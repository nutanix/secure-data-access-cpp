/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Implementation of the stateless gRPC client helpers.
 */

#include "internal/grpc_vdisk_client_support.h"

#include <grpc/impl/channel_arg_names.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/tls_certificate_verifier.h>
#include <grpcpp/security/tls_credentials_options.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace nutanix {
namespace securedata {
namespace sdk {
namespace internal {

using api::ErrorCode;
using api::ReadError;
using api::ReadHandler;
using api::ReadRegion;
using api::RecoveredChunk;
using api::VDiskClientConfig;
using api::WriteAck;
using api::WriteError;
using api::WriteHandler;
using api::WriteRegion;

namespace {

// Allow large vdisk read chunks (server max is 100 MiB).
constexpr int kMaxInboundMessageBytes = 128 * 1024 * 1024;
std::mutex kSuppressedExceptionLogMutex;

}  // namespace

std::shared_ptr<::grpc::Channel> BuildChannel(const VDiskClientConfig& config) {
  // TLS with server-certificate verification skipped: authorization is carried
  // by the bearer token, so the client trusts any server cert and needs no CA
  // material or authority override.
  ::grpc::experimental::TlsChannelCredentialsOptions tls_options;
  tls_options.set_verify_server_certs(false);
  tls_options.set_check_call_host(false);
  tls_options.set_certificate_verifier(
    std::make_shared<::grpc::experimental::NoOpCertificateVerifier>());
  std::shared_ptr<::grpc::ChannelCredentials> creds =
    ::grpc::experimental::TlsCredentials(tls_options);

  ::grpc::ChannelArguments args;
  args.SetMaxReceiveMessageSize(kMaxInboundMessageBytes);
  // Without this, every Channel to the same host:port shares one TCP/TLS
  // subchannel from the process-wide pool. Eight VDiskClients then collapse
  // onto one HTTP/2 connection, while Java Netty gives each ManagedChannel
  // its own socket.
  args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);

  // Keepalive bounds a stalled or dead stream at the transport layer instead
  // of a whole-call deadline that any large transfer would blow through. Pings
  // flow only during active calls (permit-without-calls stays off), so an idle
  // connection is never stormed. A non-positive interval leaves keepalive off.
  const int64_t keep_alive_time_ms = config.KeepAliveTime().count();
  const int64_t keep_alive_timeout_ms = config.KeepAliveTimeout().count();
  if (keep_alive_time_ms > 0 && keep_alive_timeout_ms > 0) {
    args.SetInt(
      GRPC_ARG_KEEPALIVE_TIME_MS,
      static_cast<int>(keep_alive_time_ms));
    args.SetInt(
      GRPC_ARG_KEEPALIVE_TIMEOUT_MS,
      static_cast<int>(keep_alive_timeout_ms));
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 0);
  }

  const std::string target =
    config.Host() + ":" + std::to_string(config.Port());
  return ::grpc::CreateCustomChannel(target, creds, args);
}

void ApplyBearerToken(
  ::grpc::ClientContext *context,
  const std::string& token) {
  if (!token.empty()) {
    context->AddMetadata("authorization", "Bearer " + token);
  }
}

ErrorCode ToErrorCode(int error_code_number) {
  switch (error_code_number) {
    case pb::kNoError:
      // Public ErrorCode is failure-only; a wire "no error" is not expected
      // on failure paths and is surfaced as UNKNOWN if seen.
      return ErrorCode::kUnknown;
    case pb::kInternalError:
      return ErrorCode::kInternalError;
    case pb::kInvalidArgument:
      return ErrorCode::kInvalidArgument;
    case pb::kDiskNotFound:
      return ErrorCode::kDiskNotFound;
    case pb::kDiskReadError:
      return ErrorCode::kDiskReadError;
    case pb::kDiskWriteError:
      return ErrorCode::kDiskWriteError;
    case pb::kPermissionDenied:
      return ErrorCode::kPermissionDenied;
    case pb::kConcurrentRequestNotSupported:
      return ErrorCode::kConcurrentRequestNotSupported;
    case pb::kServerBusy:
      return ErrorCode::kServerBusy;
    case pb::kRetry:
      return ErrorCode::kRetry;
    default:
      // Forward compatibility: an older SDK talking to a newer server can
      // receive a numeric code with no known constant. Surface it as kUnknown
      // (never mistaken for another reason).
      return ErrorCode::kUnknown;
  }
}

int EffectiveErrorCodeNumber(
  int mapped_number,
  const ::google::protobuf::UnknownFieldSet& unknown_fields) {
  // Guard against field-number divergence between read and write response protos.
  static_assert(
    static_cast<int>(pb::VDiskReadRet::kErrorCodeFieldNumber) ==
      static_cast<int>(pb::VDiskWriteRet::kErrorCodeFieldNumber),
    "VDiskReadRet and VDiskWriteRet must share the same error_code field "
    "number");

  int result = mapped_number;
  for (int i = 0; i < unknown_fields.field_count(); ++i) {
    const ::google::protobuf::UnknownField& field = unknown_fields.field(i);
    if (
      field.number() == pb::VDiskWriteRet::kErrorCodeFieldNumber &&
      field.type() == ::google::protobuf::UnknownField::TYPE_VARINT) {
      // Last value wins, matching proto's repeated-scalar merge rule.
      result = static_cast<int>(field.varint());
    }
  }
  return result;
}

ErrorCode TransportErrorCode(const ::grpc::Status& status) {
  switch (status.error_code()) {
    case ::grpc::StatusCode::INVALID_ARGUMENT:
    case ::grpc::StatusCode::OUT_OF_RANGE:
    case ::grpc::StatusCode::FAILED_PRECONDITION:
      return ErrorCode::kInvalidArgument;
    case ::grpc::StatusCode::NOT_FOUND:
      return ErrorCode::kDiskNotFound;
    case ::grpc::StatusCode::PERMISSION_DENIED:
    case ::grpc::StatusCode::UNAUTHENTICATED:
      return ErrorCode::kPermissionDenied;
    case ::grpc::StatusCode::RESOURCE_EXHAUSTED:
    case ::grpc::StatusCode::UNAVAILABLE:
      return ErrorCode::kServerBusy;
    case ::grpc::StatusCode::DEADLINE_EXCEEDED:
      return ErrorCode::kDeadlineExceeded;
    case ::grpc::StatusCode::CANCELLED:
      return ErrorCode::kCancelled;
    default:
      return ErrorCode::kInternalError;
  }
}

namespace {

const char *GrpcStatusName(::grpc::StatusCode code) {
  switch (code) {
    case ::grpc::StatusCode::OK:
      return "OK";
    case ::grpc::StatusCode::CANCELLED:
      return "CANCELLED";
    case ::grpc::StatusCode::UNKNOWN:
      return "UNKNOWN";
    case ::grpc::StatusCode::INVALID_ARGUMENT:
      return "INVALID_ARGUMENT";
    case ::grpc::StatusCode::DEADLINE_EXCEEDED:
      return "DEADLINE_EXCEEDED";
    case ::grpc::StatusCode::NOT_FOUND:
      return "NOT_FOUND";
    case ::grpc::StatusCode::ALREADY_EXISTS:
      return "ALREADY_EXISTS";
    case ::grpc::StatusCode::PERMISSION_DENIED:
      return "PERMISSION_DENIED";
    case ::grpc::StatusCode::UNAUTHENTICATED:
      return "UNAUTHENTICATED";
    case ::grpc::StatusCode::RESOURCE_EXHAUSTED:
      return "RESOURCE_EXHAUSTED";
    case ::grpc::StatusCode::FAILED_PRECONDITION:
      return "FAILED_PRECONDITION";
    case ::grpc::StatusCode::ABORTED:
      return "ABORTED";
    case ::grpc::StatusCode::OUT_OF_RANGE:
      return "OUT_OF_RANGE";
    case ::grpc::StatusCode::UNIMPLEMENTED:
      return "UNIMPLEMENTED";
    case ::grpc::StatusCode::INTERNAL:
      return "INTERNAL";
    case ::grpc::StatusCode::UNAVAILABLE:
      return "UNAVAILABLE";
    case ::grpc::StatusCode::DATA_LOSS:
      return "DATA_LOSS";
    default:
      return "STATUS";
  }
}

}  // namespace

std::string TransportStatusDetail(const ::grpc::Status& status) {
  std::string detail =
    "grpc_status=" + std::to_string(static_cast<int>(status.error_code())) +
    "(" + GrpcStatusName(status.error_code()) + ")";
  if (!status.error_message().empty()) {
    detail += ": " + status.error_message();
  }
  return detail;
}

void LogSuppressedException(const char *context, const char *what_message) {
  try {
    std::ostringstream line;
    line << "secure-data-access-sdk: suppressed exception in "
         << (context != nullptr ? context : "unknown");
    if (what_message != nullptr && what_message[0] != '\0') {
      line << ": " << what_message;
    }

    std::lock_guard<std::mutex> lock(kSuppressedExceptionLogMutex);
    std::cerr << line.str() << std::endl;
  } catch (...) {
  }
}

void SafeOnChunk(ReadHandler& handler, const RecoveredChunk& chunk) {
  try {
    handler.OnChunk(chunk);
  } catch (const std::exception& e) {
    LogSuppressedException("ReadHandler::OnChunk", e.what());
  } catch (...) {
    LogSuppressedException("ReadHandler::OnChunk", nullptr);
  }
}

void SafeOnError(ReadHandler& handler, const ReadError& error) {
  try {
    handler.OnError(error);
  } catch (const std::exception& e) {
    LogSuppressedException("ReadHandler::OnError", e.what());
  } catch (...) {
    LogSuppressedException("ReadHandler::OnError", nullptr);
  }
}

void SafeOnSuccess(WriteHandler& handler, const WriteAck& ack) {
  try {
    handler.OnSuccess(ack);
  } catch (const std::exception& e) {
    LogSuppressedException("WriteHandler::OnSuccess", e.what());
  } catch (...) {
    LogSuppressedException("WriteHandler::OnSuccess", nullptr);
  }
}

void SafeOnError(WriteHandler& handler, const WriteError& error) {
  try {
    handler.OnError(error);
  } catch (const std::exception& e) {
    LogSuppressedException("WriteHandler::OnError", e.what());
  } catch (...) {
    LogSuppressedException("WriteHandler::OnError", nullptr);
  }
}

bool EmitRecoveredChunks(
  const ReadRegion& region,
  pb::VDiskReadRet& value,
  ReadHandler& handler) {
  const std::string& payload = value.data();
  const int64_t payload_size = static_cast<int64_t>(payload.size());
  const int64_t region_start = region.Offset();
  const int64_t region_length = region.Length();

  // Sum the bytes the non-zero ranges claim and require it to match the
  // payload exactly: no underrun and no leftover. Summed as 64-bit so a bogus
  // set of lengths cannot overflow into a false match.
  int64_t claimed = 0;
  for (int i = 0; i < value.range_vec_size(); ++i) {
    const pb::DiskDataRange& range = value.range_vec(i);

    // Reject negative values (prevents overflow/underflow)
    if (range.offset() < 0 || range.length() < 0) {
      return false;
    }

    const int64_t chunk_start = range.offset();
    if (chunk_start < region_start) {
      return false;
    }

    // Overflow-safe in-region bounds check:
    // (chunk_start + length) <= (region_start + region_length)
    // rewritten as length <= region_length - (chunk_start - region_start).
    const int64_t from_region_start = chunk_start - region_start;
    if (
      from_region_start > region_length ||
      range.length() > region_length - from_region_start) {
      return false;
    }

    if (!range.zero_data()) {
      claimed += range.length();
    }
  }

  // Payload size accounting check
  if (claimed != payload_size) {
    return false;
  }

  // Hold one shared payload owner for every chunk emitted from this response so
  // each RecoveredChunk can be retained safely after callback return. Transfer
  // the protobuf-owned payload into that shared owner (no copy).
  std::string *released_payload = value.release_data();
  std::shared_ptr<const std::string> payload_owner(
    released_payload != nullptr ? released_payload : new std::string());

  int64_t consumed = 0;
  for (int i = 0; i < value.range_vec_size(); ++i) {
    const pb::DiskDataRange& range = value.range_vec(i);
    std::string_view data;
    if (!range.zero_data()) {
      data = std::string_view(
        payload_owner->data() + consumed,
        static_cast<size_t>(range.length()));
      consumed += range.length();
    }
    SafeOnChunk(
      handler,
      RecoveredChunk(
        range.offset(),
        range.length(),
        data,
        range.zero_data(),
        payload_owner));
  }
  return true;
}

ReadError ToReadError(
  const ReadRegion& region,
  const pb::VDiskReadRet& value,
  int error_code_number) {
  int64_t offset = region.Offset();
  int64_t length = region.Length();
  if (value.range_vec_size() > 0) {
    // Server contract: failed read responses carry exactly one range.
    const pb::DiskDataRange& range = value.range_vec(0);
    offset = range.offset();
    length = range.length();
  }
  return ReadError(
    offset,
    length,
    ToErrorCode(error_code_number),
    value.error_message());
}

pb::VDiskWriteArg BuildWriteArg(
  int64_t sequence_number,
  const WriteRegion& region,
  bool zero_data,
  api::OwnedWriteBuffer write_buffer) {
  pb::VDiskWriteArg arg;
  arg.set_sequence_number(sequence_number);
  pb::DiskDataRange *range = arg.add_range_vec();
  range->set_offset(region.Offset());
  range->set_length(region.Length());
  range->set_zero_data(zero_data);
  if (!zero_data) {
    if (write_buffer.owned_data != nullptr) {
      arg.set_allocated_data(write_buffer.owned_data.release());
    } else {
      // Fallback path for shared/non-owning buffers: proto must copy bytes.
      arg.set_data(
        write_buffer.data.data(),
        static_cast<size_t>(write_buffer.data.size()));
    }
  }
  return arg;
}

int64_t BudgetReservationFor(const WriteRegion& region) {
  return region.ZeroData() ? 0 : region.Length();
}

StreamFailure StreamFailure::Typed(ErrorCode code, std::string message) {
  StreamFailure failure;
  failure.kind_ = Kind::kTyped;
  failure.code_ = code;
  failure.message_ = std::move(message);
  return failure;
}

StreamFailure StreamFailure::Transport(ErrorCode code, std::string message) {
  StreamFailure failure;
  failure.kind_ = Kind::kTransport;
  failure.code_ = code;
  failure.message_ = std::move(message);
  return failure;
}

ReadError TerminalReadError(
  const StreamFailure& cause,
  const ReadRegion& region,
  const std::string& default_message) {
  if (cause.IsTyped()) {
    const std::string& message =
      cause.Message().empty() ? default_message : cause.Message();
    return ReadError(region.Offset(), region.Length(), cause.Code(), message);
  }
  if (cause.Present()) {
    const std::string message =
      cause.Message().empty() ? default_message
                              : default_message + " (" + cause.Message() + ")";
    return ReadError(region.Offset(), region.Length(), cause.Code(), message);
  }
  return ReadError(
    region.Offset(),
    region.Length(),
    ErrorCode::kInternalError,
    default_message);
}

WriteError TerminalWriteError(
  const StreamFailure& cause,
  const WriteRegion& region,
  const std::string& default_message) {
  if (cause.IsTyped()) {
    const std::string& message =
      cause.Message().empty() ? default_message : cause.Message();
    return WriteError(region.Offset(), region.Length(), cause.Code(), message);
  }
  if (cause.Present()) {
    const std::string message =
      cause.Message().empty() ? default_message
                              : default_message + " (" + cause.Message() + ")";
    return WriteError(region.Offset(), region.Length(), cause.Code(), message);
  }
  return WriteError(
    region.Offset(),
    region.Length(),
    ErrorCode::kInternalError,
    default_message);
}

bool StreamFailureFor(const pb::VDiskWriteRet& value, StreamFailure *out) {
  const int error_code_number = EffectiveErrorCodeNumber(
    static_cast<int>(value.error_code()),
    value.unknown_fields());
  if (value.sequence_number() == 0 && error_code_number != pb::kNoError) {
    *out = StreamFailure::Typed(
      ToErrorCode(error_code_number),
      value.error_message());
    return true;
  }
  return false;
}

void CountingSemaphore::Acquire() {
  std::unique_lock<std::mutex> lock(mutex_);
  available_.wait(lock, [this] { return permits_ > 0; });
  --permits_;
}

void CountingSemaphore::Release() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++permits_;
  }
  available_.notify_one();
}

void CountingSemaphore::Release(int count) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    permits_ += count;
  }
  available_.notify_all();
}

void ByteBudget::Acquire(int64_t bytes) {
  if (bytes <= 0) {
    return;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  has_room_.wait(lock, [this, bytes] {
    return aborted_ || used_ + bytes <= capacity_;
  });
  used_ += bytes;
}

void ByteBudget::Release(int64_t bytes) {
  if (bytes <= 0) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    used_ -= bytes;
  }
  has_room_.notify_all();
}

void ByteBudget::Abort() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = true;
  }
  has_room_.notify_all();
}

}  // namespace internal
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix
