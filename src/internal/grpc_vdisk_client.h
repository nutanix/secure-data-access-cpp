/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * gRPC-backed VDiskClient implementing the streaming read and write paths.
 */

#ifndef NUTANIX_SECUREDATA_SDK_INTERNAL_GRPC_VDISK_CLIENT_H_
#define NUTANIX_SECUREDATA_SDK_INTERNAL_GRPC_VDISK_CLIENT_H_

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/client_callback.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

#include "internal/grpc_vdisk_client_support.h"
#include "nutanix/securedata/sdk/api/read_handler.h"
#include "nutanix/securedata/sdk/api/read_region.h"
#include "nutanix/securedata/sdk/api/read_regions_request.h"
#include "nutanix/securedata/sdk/api/vdisk_client.h"
#include "nutanix/securedata/sdk/api/vdisk_client_config.h"
#include "nutanix/securedata/sdk/api/write_data_provider.h"
#include "nutanix/securedata/sdk/api/write_handler.h"
#include "nutanix/securedata/sdk/api/write_regions_request.h"
#include "vdisk.grpc.pb.h"

namespace nutanix {
namespace securedata {
namespace sdk {
namespace internal {

// gRPC-backed 'VDiskClient'.
//
// Reads use the callback 'ClientBidiReactor' API (Java Netty 'StreamObserver'
// equivalent): 'Read()' stays synchronous to the caller but caps in-flight
// streams with a per-call permit pool and does not spawn one OS thread or CQ
// poller per client. Writes still use the synchronous 'ClientReaderWriter'
// API with a reader helper thread.
class GrpcVDiskClient : public api::VDiskClient {
 public:
  // Builds a transport-backed client from high-level SDK config: creates the
  // TLS gRPC channel, stores the bearer token, and records tuning caps.
  explicit GrpcVDiskClient(const api::VDiskClientConfig& config);

  // Cancels in-flight RPCs, then blocks until every Read()/Write() caller has
  // returned and every read reactor has been destroyed.
  ~GrpcVDiskClient() override;

  void Read(const api::ReadRegionsRequest& request, api::ReadHandler& handler)
    override;

  void Write(
    const api::WriteRegionsRequest& request,
    const api::WriteDataProvider& data_provider,
    api::WriteHandler& handler) override;

  // @brief Gracefully shuts down the client and aborts any active or pending transfers.
  //
  // This method is thread-safe and idempotent. It sets the client to a closed state,
  // immediately cancels all in-flight gRPC streams (unblocking any active Read()
  // or Write() worker threads with ErrorCode::kCancelled), and prevents any new
  // requests from starting.
  //
  // @note Calling Close() is the official, supported mechanism to cancel an ongoing
  //       read or write transfer from another thread.
  // @note Do not call Close() or destroy this client from a ReadHandler /
  //       WriteHandler callback (API misuse). Let the client close when the
  //       owner destroys it after Read()/Write() returns.
  // @note Safe to call multiple times or concurrently with active operations.
  void Close() override;

 private:
  // Tracks completion and per-call concurrency for one 'Read()' call.
  struct ReadBatch {
    ReadBatch(size_t region_count, int max_concurrent_reads)
        : remaining(region_count), permits(max_concurrent_reads) {}

    void RegionDone() {
      if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lock(mutex);
        cv.notify_all();
      }
    }

    void Wait() {
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait(lock, [this] {
        return remaining.load(std::memory_order_acquire) == 0;
      });
    }

    std::atomic<size_t> remaining;
    // Cap on concurrently in-flight streams for this 'Read()' only
    // (config MaxConcurrentReads). Concurrent 'Read()' calls each own a pool.
    CountingSemaphore permits;
    std::mutex mutex;
    std::condition_variable cv;
  };

  // +1 while a Read(), Write(), or read reactor is still using the client.
  // Called only by OperationScope.
  void BeginOperation();
  void EndOperation();

  // RAII wrapper: BeginOperation() in the ctor, EndOperation() in the dtor.
  class OperationScope {
   public:
    explicit OperationScope(GrpcVDiskClient *client) : client_(client) {
      client_->BeginOperation();
    }

    ~OperationScope() { client_->EndOperation(); }

    OperationScope(const OperationScope&) = delete;
    OperationScope& operator=(const OperationScope&) = delete;

   private:
    GrpcVDiskClient *client_;
  };

  // One callback-driven VDiskStreamRead. Deleted in OnDone (gRPC contract),
  // or synchronously in AbandonBeforeCall if StartCall never ran.
  class ReadReactor
      : public ::grpc::ClientBidiReactor<pb::VDiskReadArg, pb::VDiskReadRet> {
   public:
    ReadReactor(
      GrpcVDiskClient *client,
      api::ReadHandler *handler,
      const api::ReadRegion& region,
      std::shared_ptr<ReadBatch> batch);

    void Start();

    void OnReadDone(bool ok) override;
    void OnDone(const ::grpc::Status& status) override;

   private:
    api::ReadRegion Region() const {
      return api::ReadRegion(region_offset_, region_length_);
    }

    // Sync cleanup when Start() fails before OnDone owns the reactor: release
    // permit, report error, mark region done, delete this.
    void AbandonBeforeCall(api::ErrorCode code, const char *message);

    // Records a stream-level failure without throwing out of a gRPC callback.
    void CaptureCallbackFailure(const char *message);

    // Unregisters context, releases permit, RegionDone. Does not delete this.
    // OnDone() hands off or deletes only after FinishGuard has unwound.
    void FinishReactor();

    // Unregister, release the read permit, and RegionDone. The first caller
    // does the work.
    void ReleasePermitOnce();

    // delete this. The first caller wins.
    void DeleteOnce();

    // Lives on Start()'s stack across StartCall(). OnDone marks 'deleted'
    // only after FinishGuard is gone. Start() then frees the reactor. This
    // object stays.
    struct StartupState {
      std::atomic<bool> deleted{false};
    };

    // First member so it is destroyed last, after 'context_'. Read()'s own
    // OperationScope can end at RegionDone, which runs before 'delete this';
    // this count keeps the client alive through ClientContext teardown.
    [[maybe_unused]] OperationScope reactor_operation_;

    GrpcVDiskClient *client_;
    api::ReadHandler *handler_;
    int64_t region_offset_;
    int64_t region_length_;
    std::shared_ptr<ReadBatch> batch_;
    ::grpc::ClientContext context_;
    pb::VDiskReadArg arg_;
    pb::VDiskReadRet response_;
    StreamFailure failure_;
    bool context_registered_ = false;
    // Non-null only while Start() is inside StartCall(). OnDone publishes
    // "deleted" here only after FinishGuard has unwound, then Start()
    // deletes. A later OnDone sees null, deletes this, and does not touch
    // the stack.
    std::atomic<StartupState *> startup_state_{nullptr};
    // Permit and RegionDone have been published.
    std::atomic<bool> permit_released_{false};
    // delete this has been claimed.
    std::atomic<bool> delete_claimed_{false};
  };

  // Applies the bearer token and, when configured, the overall deadline to a
  // fresh call context.
  void PrepareContext(::grpc::ClientContext *context);

  // Opens one callback read stream for 'region'. Caller already holds a
  // per-call read permit; the reactor releases it in OnDone.
  void StartReactorRead(
    const api::ReadRegion& region,
    api::ReadHandler& handler,
    const std::shared_ptr<ReadBatch>& batch);

  // Removes 'context' from 'active_contexts_' and wakes the destructor wait
  // when no callers and no contexts remain. Last safe use of client state for
  // that stream.
  void UnregisterActiveContext(::grpc::ClientContext *context);

  // True when no callers/reactors are active and no RPC context is registered.
  // Caller already holds contexts_mutex_.
  bool IsIdleLocked() const;

  // Destructor wait: blocks until IsIdleLocked() is true.
  void WaitUntilIdle();

  // TLS channel shared by every call. Outlives 'Close()': closing only
  // cancels in-flight calls via 'active_contexts_' and blocks new ones via
  // 'closed_'; the channel itself is torn down when the client is destroyed.
  std::shared_ptr<::grpc::Channel> channel_;

  // Service stub bound to 'channel_', also retained for this object's lifetime.
  std::unique_ptr<pb::VDiskDataSvc::Stub> stub_;

  // Bearer token identifying the caller and the target disk.
  std::string auth_token_;

  // Optional hard cap on the whole operation, applied as the gRPC call
  // deadline (0 = unbounded). Stream liveness is otherwise bounded by channel
  // keepalive.
  int64_t overall_deadline_ms_;

  // Per-Read() cap on concurrently in-flight streams (config MaxConcurrentReads).
  int max_concurrent_reads_;

  // Server-derived write caps.
  int64_t max_write_bytes_per_request_;
  int max_in_flight_write_requests_;
  int64_t max_in_flight_write_bytes_;

  // Once set, no new operations may start and all in-flight contexts are
  // cancelled via TryCancel().
  std::atomic<bool> closed_{false};

  // Held by the write sender from its closed_ check through stream->Write().
  // Close() calls TryCancel() first, then takes this mutex. Cancel makes a
  // blocked Write() return and drop the lock. A sender that has not entered
  // the check yet then sees closed_ and does not send.
  std::mutex send_mu_;

  // Live ClientContext pointers for every in-flight read stream and write
  // call. Close() cancels these. Recursive so Close() can TryCancel under the
  // lock while a sync OnDone → UnregisterActiveContext re-enters safely.
  // Still do not Close()/destroy the client from a handler callback (API misuse).
  //
  // The destructor waits until 'active_contexts_' is empty AND
  // 'active_operations_' is zero. Each Read()/Write() caller and each read
  // reactor holds a count. Contexts alone do not cover a caller that has not
  // registered its first RPC yet, or (with MaxConcurrentReads(1)) the gap
  // after one region unregisters and before the next is registered.
  std::recursive_mutex contexts_mutex_;
  std::condition_variable_any contexts_cv_;
  std::unordered_set<::grpc::ClientContext *> active_contexts_;
  size_t active_operations_ = 0;

  // SDK-internal write correlation ids; start at 1 so that a 0 sequence marks
  // an unsequenced server admission rejection.
  std::atomic<int64_t> sequence_counter_{1};
};

}  // namespace internal
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_INTERNAL_GRPC_VDISK_CLIENT_H_
