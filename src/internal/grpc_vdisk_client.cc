/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Implementation of the gRPC-backed streaming read and write paths.
 */

#include "internal/grpc_vdisk_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "internal/grpc_vdisk_client_support.h"
#include "internal/grpc_vdisk_client_test_seam.h"
#include "nutanix/securedata/sdk/api/read_error.h"
#include "nutanix/securedata/sdk/api/write_ack.h"
#include "nutanix/securedata/sdk/api/write_error.h"

namespace nutanix {
namespace securedata {
namespace sdk {
namespace internal {

using api::ErrorCode;
using api::ReadError;
using api::ReadHandler;
using api::ReadRegion;
using api::ReadRegionsRequest;
using api::VDiskClientConfig;
using api::WriteAck;
using api::WriteDataProvider;
using api::WriteError;
using api::WriteHandler;
using api::WriteRegion;
using api::WriteRegionsRequest;

namespace {

// Installed once, before tests run. Production leaves this null.
std::atomic<const GrpcVDiskClientTestHooks *> g_test_hooks{nullptr};

const GrpcVDiskClientTestHooks *TestHooks() {
  return g_test_hooks.load(std::memory_order_acquire);
}

}  // namespace

void SetGrpcVDiskClientTestHooks(const GrpcVDiskClientTestHooks *hooks) {
  g_test_hooks.store(hooks, std::memory_order_release);
}

void WaitIfPauseBeforeRpcArmed() {
  const GrpcVDiskClientTestHooks *hooks = TestHooks();
  if (hooks != nullptr && hooks->wait_if_pause_before_rpc != nullptr) {
    hooks->wait_if_pause_before_rpc();
  }
}

bool FailReaderThreadStartForTest() {
  const GrpcVDiskClientTestHooks *hooks = TestHooks();
  return hooks != nullptr && hooks->fail_reader_thread_start != nullptr &&
         hooks->fail_reader_thread_start();
}

void NoteIdleWaitBlockedForTest() {
  const GrpcVDiskClientTestHooks *hooks = TestHooks();
  if (hooks != nullptr && hooks->note_idle_wait_blocked != nullptr) {
    hooks->note_idle_wait_blocked();
  }
}

void NoteIdleWaitUnblockedForTest() {
  const GrpcVDiskClientTestHooks *hooks = TestHooks();
  if (hooks != nullptr && hooks->note_idle_wait_unblocked != nullptr) {
    hooks->note_idle_wait_unblocked();
  }
}

namespace {

// Thread-safe, first-wins holder for the whole-stream failure reason, shared
// between a write's reader thread and its sender thread.
class SharedFailure {
 public:
  // Records 'failure' only if none has been captured yet (first-wins).
  bool CaptureIfAbsent(const StreamFailure& failure) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!failure_.Present()) {
      failure_ = failure;
      return true;
    }
    return false;
  }

  bool Present() {
    std::lock_guard<std::mutex> lock(mutex_);
    return failure_.Present();
  }

  StreamFailure Get() {
    std::lock_guard<std::mutex> lock(mutex_);
    return failure_;
  }

 private:
  std::mutex mutex_;
  StreamFailure failure_;
};

}  // namespace

GrpcVDiskClient::GrpcVDiskClient(const VDiskClientConfig& config)
    : channel_(BuildChannel(config)),
      stub_(pb::VDiskDataSvc::NewStub(channel_)),
      auth_token_(config.AuthToken()),
      overall_deadline_ms_(config.OverallDeadline().count()),
      max_concurrent_reads_(config.MaxConcurrentReads()),
      max_write_bytes_per_request_(config.MaxWriteBytesPerRequest()),
      max_in_flight_write_requests_(config.MaxInFlightWriteRequests()),
      max_in_flight_write_bytes_(config.MaxInFlightWriteBytes()) {}

GrpcVDiskClient::~GrpcVDiskClient() {
  Close();
  // Close() only cancels. A Read()/Write() caller can still be running with
  // nothing in active_contexts_ (before the first RPC, or between regions),
  // so wait until that caller has returned as well.
  WaitUntilIdle();
}

void GrpcVDiskClient::UnregisterActiveContext(::grpc::ClientContext *context) {
  std::lock_guard<std::recursive_mutex> lock(contexts_mutex_);
  active_contexts_.erase(context);
  if (IsIdleLocked()) {
    contexts_cv_.notify_all();
  }
}

// Marks one Read(), Write(), or read reactor as in progress. Called only by
// OperationScope.
void GrpcVDiskClient::BeginOperation() {
  std::lock_guard<std::recursive_mutex> lock(contexts_mutex_);
  ++active_operations_;
}

// Drops that in-progress mark. Wakes the destructor if nothing else is left.
void GrpcVDiskClient::EndOperation() {
  std::lock_guard<std::recursive_mutex> lock(contexts_mutex_);
  --active_operations_;
  if (IsIdleLocked()) {
    contexts_cv_.notify_all();
  }
}

// True when no Read()/Write() is running and no RPC context is registered.
// Caller already holds contexts_mutex_.
bool GrpcVDiskClient::IsIdleLocked() const {
  return active_operations_ == 0 && active_contexts_.empty();
}

// Used by the destructor: wait until IsIdleLocked() is true so callers and
// reactors cannot touch a destroyed client.
void GrpcVDiskClient::WaitUntilIdle() {
  std::unique_lock<std::recursive_mutex> lock(contexts_mutex_);
  if (IsIdleLocked()) {
    return;
  }
  NoteIdleWaitBlockedForTest();
  contexts_cv_.wait(lock, [this] { return IsIdleLocked(); });
  NoteIdleWaitUnblockedForTest();
}

void GrpcVDiskClient::PrepareContext(::grpc::ClientContext *context) {
  ApplyBearerToken(context, auth_token_);
  // If an overall deadline is configured, attach it so the whole call fails
  // with DEADLINE_EXCEEDED once it elapses. Otherwise channel keepalive is
  // what bounds a stalled or dead transfer.
  if (overall_deadline_ms_ > 0) {
    context->set_deadline(
      std::chrono::system_clock::now() +
      std::chrono::milliseconds(overall_deadline_ms_));
  }
}

GrpcVDiskClient::ReadReactor::ReadReactor(
  GrpcVDiskClient *client,
  api::ReadHandler *handler,
  const api::ReadRegion& region,
  std::shared_ptr<ReadBatch> batch)
    : reactor_operation_(client),
      client_(client),
      handler_(handler),
      region_offset_(region.Offset()),
      region_length_(region.Length()),
      batch_(std::move(batch)) {
  arg_.set_offset(region_offset_);
  arg_.set_length(region_length_);
}

void GrpcVDiskClient::ReadReactor::ReleasePermitOnce() {
  if (permit_released_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  // Unregister before waking Read(). Release/RegionDone can resume the caller,
  // which may touch the client and, on the last region, return. This reactor
  // must not use the client after that. The caller's operation count, not this
  // context, keeps the object alive until Read() actually returns, including
  // the gap before the next region's context is registered.
  if (context_registered_) {
    client_->UnregisterActiveContext(&context_);
    context_registered_ = false;
  }
  batch_->permits.Release();
  batch_->RegionDone();
}

void GrpcVDiskClient::ReadReactor::DeleteOnce() {
  if (!delete_claimed_.exchange(true, std::memory_order_acq_rel)) {
    delete this;
  }
}

void GrpcVDiskClient::ReadReactor::AbandonBeforeCall(
  ErrorCode code,
  const char *message) {
  // SafeOnError already swallows handler exceptions.
  SafeOnError(
    *handler_,
    ReadError(region_offset_, region_length_, code, message));
  ReleasePermitOnce();
  DeleteOnce();
}

void GrpcVDiskClient::ReadReactor::CaptureCallbackFailure(const char *message) {
  if (failure_.Present()) {
    return;
  }
  try {
    failure_ = StreamFailure::Typed(
      ErrorCode::kInternalError,
      message != nullptr ? message : "read callback failed");
  } catch (...) {
  }
}

void GrpcVDiskClient::ReadReactor::FinishReactor() {
  // Do not mark StartupState or delete this here. FinishReactor runs from
  // ~FinishGuard, which is still unwinding inside OnDone(). OnDone publishes
  // the handoff only after this guard is gone.
  ReleasePermitOnce();
}

void GrpcVDiskClient::ReadReactor::Start() {
  // Mirror the old worker try/catch: never let an exception escape before the
  // permit/RegionDone are handled. Everything in here runs before gRPC owns
  // the reactor, so a failure cleans up synchronously.
  try {
    client_->PrepareContext(&context_);
    // Hold contexts_mutex_ only to update the set — never call user handlers
    // (or Close()) under it, or a handler that calls Close() deadlocks.
    bool closed = false;
    {
      std::lock_guard<std::recursive_mutex> lock(client_->contexts_mutex_);
      closed = client_->closed_.load();
      if (!closed) {
        client_->active_contexts_.insert(&context_);
        context_registered_ = true;
      }
    }
    if (closed) {
      AbandonBeforeCall(ErrorCode::kCancelled, "Client closed");
      return;
    }
    // binding this reactor to the RPC
    client_->stub_->async()->VDiskStreamRead(&context_, this);
    // send the read request and half-close client→server
    StartWriteLast(&arg_, ::grpc::WriteOptions());
    // when the next message arrives, put it in response_ and call OnReadDone
    StartRead(&response_);
  } catch (const std::exception& e) {
    AbandonBeforeCall(ErrorCode::kInternalError, e.what());
    return;
  } catch (...) {
    AbandonBeforeCall(ErrorCode::kInternalError, "failed to start read stream");
    return;
  }

  // StartCall() hands this reactor to gRPC, which then calls OnDone() exactly
  // once. gRPC aborts the process rather than throwing if the call cannot
  // start, so it stays outside the try: from here on only OnDone() frees this.
  //
  // OnDone() may run inside StartCall(). The guard lives on this stack so
  // OnDone() can report that it has finished without freeing this while
  // StartCall() is still on the stack. Start() then deletes.
  StartupState state;
  startup_state_.store(&state, std::memory_order_release);
  StartCall();
  if (startup_state_.exchange(nullptr, std::memory_order_acq_rel) != nullptr) {
    // OnDone() has not run yet. It will see null and delete this itself.
    return;
  }
  // OnDone() took the guard. Wait until it has fully returned, then delete.
  while (!state.deleted.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  DeleteOnce();
}

// Read-ret message arrived
void GrpcVDiskClient::ReadReactor::OnReadDone(bool ok) {
  if (!ok) {
    return;
  }
  // Must not throw out of a gRPC callback: that can skip StartRead and leave
  // the stream (and Read() permit/batch wait) wedged.
  try {
    const int error_code_number = EffectiveErrorCodeNumber(
      static_cast<int>(response_.error_code()),
      response_.unknown_fields());
    if (error_code_number != pb::kNoError) {
      if (response_.range_vec_size() == 0) {
        if (!failure_.Present()) {
          failure_ = StreamFailure::Typed(
            ToErrorCode(error_code_number),
            response_.error_message());
        }
      } else {
        SafeOnError(
          *handler_,
          ToReadError(Region(), response_, error_code_number));
      }
    } else if (!EmitRecoveredChunks(Region(), response_, *handler_)) {
      SafeOnError(
        *handler_,
        ReadError(
          region_offset_,
          region_length_,
          ErrorCode::kInternalError,
          "Malformed read response: payload/range validation failed"));
    }
  } catch (const std::exception& e) {
    SafeOnError(
      *handler_,
      ReadError(
        region_offset_,
        region_length_,
        ErrorCode::kInternalError,
        "read callback failed"));
  } catch (...) {
    SafeOnError(
      *handler_,
      ReadError(
        region_offset_,
        region_length_,
        ErrorCode::kInternalError,
        "read callback failed"));
  }
  try {
    response_.Clear();
    StartRead(&response_);
  } catch (const std::exception& e) {
    CaptureCallbackFailure(e.what());
    context_.TryCancel();
  } catch (...) {
    CaptureCallbackFailure("failed to continue read stream");
    context_.TryCancel();
  }
}

void GrpcVDiskClient::ReadReactor::OnDone(const ::grpc::Status& status) {
  // FinishGuard must be fully destroyed before the startup handoff. Start()
  // deletes this as soon as it sees the flag, so the flag cannot be set
  // while ~FinishGuard is still running.
  {
    // C++ has no finally; RAII runs FinishReactor even if reporting throws.
    struct FinishGuard {
      ReadReactor *self;
      ~FinishGuard() { self->FinishReactor(); }
    } finish_guard{this};

    try {
      if (failure_.Present()) {
        SafeOnError(
          *handler_,
          TerminalReadError(failure_, Region(), "Read stream failed"));
      } else if (!status.ok()) {
        SafeOnError(
          *handler_,
          ReadError(
            region_offset_,
            region_length_,
            TransportErrorCode(status),
            "Read stream error: " + TransportStatusDetail(status)));
      }
    } catch (...) {
      SafeOnError(
        *handler_,
        ReadError(
          region_offset_,
          region_length_,
          ErrorCode::kInternalError,
          "read completion failed"));
    }
  }

  // FinishGuard is gone and OnDone has no further use of this. If Start()
  // is inside StartCall(), mark its stack and let it delete after StartCall()
  // returns. Otherwise this OnDone is later and deletes here.
  if (
    StartupState *state =
      startup_state_.exchange(nullptr, std::memory_order_acq_rel)) {
    state->deleted.store(true, std::memory_order_release);
    return;
  }
  DeleteOnce();
}

void GrpcVDiskClient::StartReactorRead(
  const ReadRegion& region,
  ReadHandler& handler,
  const std::shared_ptr<ReadBatch>& batch) {
  // Caller already holds a permit. If construction fails, give it back here.
  // If Start() fails before StartCall(), the reactor abandons itself.
  ReadReactor *reactor = nullptr;
  try {
    reactor = new ReadReactor(this, &handler, region, batch);
  } catch (const std::exception& e) {
    batch->permits.Release();
    SafeOnError(
      handler,
      ReadError(
        region.Offset(),
        region.Length(),
        ErrorCode::kInternalError,
        std::string("failed to start read: ") + e.what()));
    batch->RegionDone();
    return;
  } catch (...) {
    batch->permits.Release();
    SafeOnError(
      handler,
      ReadError(
        region.Offset(),
        region.Length(),
        ErrorCode::kInternalError,
        "failed to start read"));
    batch->RegionDone();
    return;
  }
  reactor->Start();
}

void GrpcVDiskClient::Read(
  const ReadRegionsRequest& request,
  ReadHandler& handler) {
  // Held until this function returns, including while no RPC context exists.
  [[maybe_unused]] OperationScope operation(this);

  const std::vector<ReadRegion>& regions = request.Regions();
  if (regions.empty()) {
    return;
  }
  if (closed_.load()) {
    for (size_t i = 0; i < regions.size(); ++i) {
      const ReadRegion& region = regions[i];
      SafeOnError(
        handler,
        ReadError(
          region.Offset(),
          region.Length(),
          ErrorCode::kCancelled,
          "Client closed"));
    }
    return;
  }

  // Walk the region list on the calling thread, blocking for a per-call
  // permit before launching each callback stream. Concurrent Read() calls
  // each own an independent pool sized by MaxConcurrentReads.
  auto batch =
    std::make_shared<ReadBatch>(regions.size(), max_concurrent_reads_);
  for (size_t i = 0; i < regions.size(); ++i) {
    if (closed_.load()) {
      for (; i < regions.size(); ++i) {
        const ReadRegion& region = regions[i];
        SafeOnError(
          handler,
          ReadError(
            region.Offset(),
            region.Length(),
            ErrorCode::kCancelled,
            "Client closed"));
        batch->RegionDone();
      }
      break;
    }
    batch->permits.Acquire();
    // Previous region (if any) has already unregistered: its permit is released
    // only after UnregisterActiveContext. Nothing is registered again until
    // StartReactorRead below. Tests can pause here to hold that gap. No-op
    // in production.
    WaitIfPauseBeforeRpcArmed();
    if (closed_.load()) {
      batch->permits.Release();
      for (; i < regions.size(); ++i) {
        const ReadRegion& region = regions[i];
        SafeOnError(
          handler,
          ReadError(
            region.Offset(),
            region.Length(),
            ErrorCode::kCancelled,
            "Client closed"));
        batch->RegionDone();
      }
      break;
    }
    StartReactorRead(regions[i], handler, batch);
  }

  // this calling thread doesn’t return from Read()
  // until every region in this request is done
  batch->Wait();
}

void GrpcVDiskClient::Write(
  const WriteRegionsRequest& request,
  const WriteDataProvider& data_provider,
  WriteHandler& handler) {
  // Held until this function returns. Declared before the ClientContext so
  // that context is destroyed first and the operation count drops last.
  [[maybe_unused]] OperationScope operation(this);
  // Before the closed check and before the context is inserted: tests can
  // block here to hold that gap. No-op in production.
  WaitIfPauseBeforeRpcArmed();

  const std::vector<WriteRegion>& regions = request.Regions();
  if (closed_.load()) {
    for (size_t i = 0; i < regions.size(); ++i) {
      const WriteRegion& region = regions[i];
      SafeOnError(
        handler,
        WriteError(
          region.Offset(),
          region.Length(),
          ErrorCode::kCancelled,
          "Client closed"));
    }
    return;
  }

  // Two independent gates bound what is in flight (sent but unacked): at most
  // 'max_in_flight_write_requests_' requests by count, and at most
  // 'max_in_flight_write_bytes_' of aggregate payload by size. The sender
  // blocks for both before pulling the next region's bytes, so an unbounded
  // region list is queued rather than rejected and peak client memory is
  // bounded by the byte budget.
  CountingSemaphore in_flight(max_in_flight_write_requests_);
  ByteBudget byte_budget(max_in_flight_write_bytes_);

  // seq -> region: the sequence number is an SDK-internal correlation id; the
  // region's offset and length are the user-facing identity. Guarded because
  // the sender thread inserts while the reader thread removes.
  std::unordered_map<int64_t, WriteRegion> pending;
  std::mutex pending_mutex;

  SharedFailure failure;
  // Set true by the reader thread at its terminal, before it releases every
  // permit, so a sender woken by that release bails instead of sending onto a
  // dead stream.
  std::atomic<bool> reader_done{false};

  ::grpc::ClientContext context;
  PrepareContext(&context);
  bool cancelled = false;

  // Double-check under the lock: Close() may have raced the fast-exit above.
  // Only update active_contexts_ under the lock; SafeOnError runs after unlock.
  {
    std::lock_guard<std::recursive_mutex> lock(contexts_mutex_);
    if (closed_.load()) {
      cancelled = true;
    } else {
      active_contexts_.insert(&context);
    }
  }
  if (cancelled) {
    for (size_t i = 0; i < regions.size(); ++i) {
      const WriteRegion& region = regions[i];
      SafeOnError(
        handler,
        WriteError(
          region.Offset(),
          region.Length(),
          ErrorCode::kCancelled,
          "Client closed"));
    }
    return;
  }

  struct ContextScope {
    GrpcVDiskClient *client = nullptr;
    ::grpc::ClientContext *context = nullptr;

    // Drop this call from the client-wide set before OperationScope ends, so
    // the destructor wait sees the context go away while the caller count
    // still keeps the client alive. Lock only for the set update — no user
    // callbacks here.
    ~ContextScope() { client->UnregisterActiveContext(context); }
  } context_scope{this, &context};

  // opens bi-di stream
  std::unique_ptr<
    ::grpc::ClientReaderWriter<pb::VDiskWriteArg, pb::VDiskWriteRet>>
    stream = stub_->VDiskStreamWrite(&context);

  std::thread reader;
  try {
    if (FailReaderThreadStartForTest()) {
      throw std::system_error(
        std::make_error_code(std::errc::resource_unavailable_try_again),
        "forced reader thread start failure");
    }
    // Reader thread: drains acknowledgements until the stream terminates. gRPC
    // guarantees no further read after 'Read' returns false, so once it exits
    // the sender can safely drain 'pending' without racing an ack.
    reader = std::thread([&] {
      pb::VDiskWriteRet response;
      while (stream->Read(&response)) {
        std::optional<WriteRegion> region;
        {
          std::lock_guard<std::mutex> lock(pending_mutex);
          auto it = pending.find(response.sequence_number());
          if (it != pending.end()) {
            region = it->second;
            pending.erase(it);
          }
        }
        if (!region.has_value()) {
          // No region owns this response. An unsequenced admission rejection
          // ends the whole stream: record that typed reason (first-wins) so the
          // drain reports it for every unacked region. Anything else is a stray
          // or duplicate delivery and is only logged.
          StreamFailure captured;
          if (StreamFailureFor(response, &captured)) {
            if (failure.CaptureIfAbsent(captured)) {
              // Refusal captured while the stream is still open: wake a sender
              // that might be blocked on either gate so it can observe the
              // failure immediately instead of waiting for stream terminal.
              in_flight.Release(max_in_flight_write_requests_);
              byte_budget.Abort();
            }
          }
          response.Clear();
          continue;
        }
        // Free the request slot and the region's byte reservation first so the
        // sender can enqueue the next queued region regardless of the handler.
        in_flight.Release();
        byte_budget.Release(BudgetReservationFor(*region));
        const int error_code_number = EffectiveErrorCodeNumber(
          static_cast<int>(response.error_code()),
          response.unknown_fields());
        if (error_code_number == pb::kNoError) {
          SafeOnSuccess(handler, WriteAck(region->Offset(), region->Length()));
        } else {
          SafeOnError(
            handler,
            WriteError(
              region->Offset(),
              region->Length(),
              ToErrorCode(error_code_number),
              response.error_message()));
        }
        // Clears response buffer for next
        response.Clear();
      }
      // Terminal: no ack can follow. Wake any blocked sender and let it bail.
      reader_done.store(true);
      in_flight.Release(max_in_flight_write_requests_);
      byte_budget.Abort();
    });
  } catch (const std::system_error& e) {
    context.TryCancel();
    stream->Finish();
    for (size_t i = 0; i < regions.size(); ++i) {
      const WriteRegion& region = regions[i];
      SafeOnError(
        handler,
        WriteError(
          region.Offset(),
          region.Length(),
          ErrorCode::kInternalError,
          "failed to start write reader thread: " + std::string(e.what())));
    }
    return;
  }

  // Index of the next region to process. Everything before it has been
  // accounted for (reported as a drop, or handed to 'pending'); everything
  // from it onward is unsent. The drain uses it to fail the unsent tail so
  // every submitted region gets exactly one terminal callback.
  size_t send_index = 0;
  for (; send_index < regions.size(); ++send_index) {
    const WriteRegion& region = regions[send_index];

    // Reject an oversized data region up front, before taking either gate or
    // pulling any bytes. The server would reject it with kInvalidArgument, so
    // fail just this region here and keep the stream healthy for the rest.
    if (!region.ZeroData() && region.Length() > max_write_bytes_per_request_) {
      SafeOnError(
        handler,
        WriteError(
          region.Offset(),
          region.Length(),
          ErrorCode::kInvalidArgument,
          "write region length " + std::to_string(region.Length()) +
            " exceeds max write bytes per request " +
            std::to_string(max_write_bytes_per_request_)));
      continue;
    }

    // Take a request slot, then reserve the region's bytes, before pulling
    // them. Both acquires can block; if the stream is torn down while waiting,
    // the reader's release-all and abort wake us.
    in_flight.Acquire();
    const int64_t reservation = BudgetReservationFor(region);
    byte_budget.Acquire(reservation);

    // One post-acquire check covers both waits. Stop if a failure was
    // recorded, the reader finished, or Close() set closed_. Close() is
    // visible here before the reader thread notices the cancel. 'send_index'
    // still points at this region, so the drain fails it and the rest as
    // unsent.
    if (failure.Present() || reader_done.load() || closed_.load()) {
      byte_budget.Release(reservation);
      in_flight.Release();
      break;
    }

    // Zero-fill regions carry no bytes, so the provider is skipped. For data
    // regions, pull the bytes now (one request's worth) and validate them.
    const bool zero_fill = region.ZeroData();
    api::OwnedWriteBuffer write_buffer;
    if (!zero_fill) {
      // Close() may have landed after the check above. Do not pull bytes.
      if (closed_.load()) {
        byte_budget.Release(reservation);
        in_flight.Release();
        break;
      }
      bool provider_failed = false;
      try {
        write_buffer = data_provider(region.Offset(), region.Length());
      } catch (const std::exception& e) {
        LogSuppressedException("WriteDataProvider", e.what());
        SafeOnError(
          handler,
          WriteError(
            region.Offset(),
            region.Length(),
            ErrorCode::kInternalError,
            std::string("data provider failed: ") + e.what()));
        provider_failed = true;
      } catch (...) {
        LogSuppressedException("WriteDataProvider", nullptr);
        SafeOnError(
          handler,
          WriteError(
            region.Offset(),
            region.Length(),
            ErrorCode::kInternalError,
            "data provider failed"));
        provider_failed = true;
      }
      if (provider_failed) {
        byte_budget.Release(reservation);
        in_flight.Release();
        continue;
      }
      // The provider must return exactly region.Length() bytes.
      if (static_cast<int64_t>(write_buffer.data.size()) != region.Length()) {
        SafeOnError(
          handler,
          WriteError(
            region.Offset(),
            region.Length(),
            ErrorCode::kInvalidArgument,
            "data provider returned " +
              std::to_string(write_buffer.data.size()) +
              " bytes for region of length " +
              std::to_string(region.Length())));
        byte_budget.Release(reservation);
        in_flight.Release();
        continue;
      }
    }

    // Check and send under send_mu_. Close() sets closed_, cancels, then
    // waits for this mutex, so a sender that has not entered here does not
    // send. The region is inserted only when we actually send, so a stop
    // here leaves it for the unsent-tail drain exactly once.
    {
      std::lock_guard<std::mutex> send_lock(send_mu_);
      if (closed_.load()) {
        byte_budget.Release(reservation);
        in_flight.Release();
        break;
      }
      const int64_t sequence_number =
        sequence_counter_.fetch_add(1, std::memory_order_relaxed);
      {
        std::lock_guard<std::mutex> lock(pending_mutex);
        pending.emplace(sequence_number, region);
      }
      const pb::VDiskWriteArg arg = BuildWriteArg(
        sequence_number,
        region,
        zero_fill,
        std::move(write_buffer));
      if (!stream->Write(arg)) {
        // The stream is broken; the reader will end and Finish yields the real
        // status. This region is owned by 'pending' and will be reported by the
        // drain with the captured cause. Stop sending the rest.
        ++send_index;
        break;
      }
    }
  }

  // Half-close so the server can drain responses and cleanly finish, but only
  // if we actually sent the whole list; if we stopped early, force the stream
  // to terminate so the reader thread unblocks and Finish returns.
  if (send_index == regions.size()) {
    stream->WritesDone();
  } else {
    context.TryCancel();
  }

  // Wait for the reader to observe the transport terminal before draining, so
  // it cannot race the drain into double-reporting a region.
  reader.join();

  const ::grpc::Status status = stream->Finish();
  // Record the transport reason if nothing more specific was captured, so the
  // drain fails unacked regions with the real cause.
  if (!status.ok()) {
    const StreamFailure transport_failure = StreamFailure::Transport(
      TransportErrorCode(status),
      TransportStatusDetail(status));
    failure.CaptureIfAbsent(transport_failure);
  }

  const StreamFailure cause = failure.Get();

  // Guarantee exactly-once reporting for every submitted region: regions
  // handed to the server but never acked live in 'pending'; regions never sent
  // are the tail from 'send_index'. The two sets are disjoint. The reader is
  // joined, so no ack can race this drain.
  {
    std::lock_guard<std::mutex> lock(pending_mutex);
    for (const auto& entry : pending) {
      SafeOnError(
        handler,
        TerminalWriteError(cause, entry.second, "No write response received"));
    }
    pending.clear();
  }
  for (size_t i = send_index; i < regions.size(); ++i) {
    SafeOnError(
      handler,
      TerminalWriteError(
        cause,
        regions[i],
        "Write stream closed before region was sent"));
  }
}

void GrpcVDiskClient::Close() {
  closed_.store(true);
  // Cancel before taking send_mu_. A sender may be blocked inside
  // stream->Write() while holding that mutex. TryCancel() makes that Write()
  // return so the sender can drop the lock. Waiting for the lock first would
  // leave Close() stuck behind the write it is trying to unblock.
  //
  // Snapshot + TryCancel under the same lock so OnDone cannot free a
  // ClientContext before cancel runs. Recursive mutex lets sync OnDone →
  // UnregisterActiveContext re-enter. Iterate a copy: Unregister may erase
  // from active_contexts_ while we cancel.
  {
    std::lock_guard<std::recursive_mutex> lock(contexts_mutex_);
    std::vector<::grpc::ClientContext *> to_cancel(
      active_contexts_.begin(),
      active_contexts_.end());
    for (size_t i = 0; i < to_cancel.size(); ++i) {
      to_cancel[i]->TryCancel();
    }
  }
  // A sender blocked in stream->Write() can now finish and release send_mu_.
  // Taking the lock waits for that. A sender still outside the check then
  // sees closed_.
  std::lock_guard<std::mutex> send_lock(send_mu_);
}

}  // namespace internal
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix
