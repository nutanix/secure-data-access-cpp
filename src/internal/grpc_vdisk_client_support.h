/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Stateless helpers for the gRPC client: channel setup, error mapping, and
 * flow-control gates.
 */

#ifndef NUTANIX_SECUREDATA_SDK_INTERNAL_GRPC_VDISK_CLIENT_SUPPORT_H_
#define NUTANIX_SECUREDATA_SDK_INTERNAL_GRPC_VDISK_CLIENT_SUPPORT_H_

#include <google/protobuf/unknown_field_set.h>
#include <grpcpp/grpcpp.h>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "nutanix/securedata/sdk/api/error_code.h"
#include "nutanix/securedata/sdk/api/read_error.h"
#include "nutanix/securedata/sdk/api/read_handler.h"
#include "nutanix/securedata/sdk/api/read_region.h"
#include "nutanix/securedata/sdk/api/recovered_chunk.h"
#include "nutanix/securedata/sdk/api/vdisk_client_config.h"
#include "nutanix/securedata/sdk/api/write_ack.h"
#include "nutanix/securedata/sdk/api/write_data_provider.h"
#include "nutanix/securedata/sdk/api/write_error.h"
#include "nutanix/securedata/sdk/api/write_handler.h"
#include "nutanix/securedata/sdk/api/write_region.h"
#include "vdisk.grpc.pb.h"
#include "vdisk.pb.h"

namespace nutanix {
namespace securedata {
namespace sdk {
namespace internal {

// The wire contract declares no proto package (see vdisk.proto), which is what
// makes the gRPC service reachable as "/VDiskDataSvc/..." on the server. As a
// result the generated stubs live in the global namespace. Re-export the
// symbols the SDK uses under a stable 'pb' alias so call sites stay unchanged.
namespace pb {
using ::DiskDataRange;
using ::kConcurrentRequestNotSupported;
using ::kDiskNotFound;
using ::kDiskReadError;
using ::kDiskWriteError;
using ::kInternalError;
using ::kInvalidArgument;
using ::kNoError;
using ::kPermissionDenied;
using ::kRetry;
using ::kServerBusy;
using ::VDiskDataSvc;
using ::VDiskErrorCode;
using ::VDiskReadArg;
using ::VDiskReadRet;
using ::VDiskWriteArg;
using ::VDiskWriteRet;
}  // namespace pb

// Creates a TLS gRPC channel with server-certificate verification skipped.
// Authorization is carried by the bearer token, not the server certificate, so
// the client trusts any server cert and needs no CA material or authority
// override. Keepalive and a fixed internal max inbound message size are
// applied.
std::shared_ptr<::grpc::Channel> BuildChannel(
  const api::VDiskClientConfig& config);

// Adds the bearer 'token' to 'context' as "authorization: Bearer <token>". No
// header is added when 'token' is empty.
void ApplyBearerToken(::grpc::ClientContext *context, const std::string& token);

// Maps a raw 'error_code' wire value to the SDK's public 'ErrorCode'. Any
// value this SDK build does not recognize -- including a newer code -- is
// reported as 'kUnknown' so the public API stays stable when the server adds
// new codes.
api::ErrorCode ToErrorCode(int error_code_number);

// The effective 'error_code' wire value, robust to proto2 closed-enum
// behavior. A value newer than this build's enum is dropped into the message's
// unknown-field set and 'error_code()' then reports the default 'kNoError'.
// This reads the raw value back from the unknown-field set when present, so an
// unrecognized code is surfaced (as 'kUnknown') instead of a false success.
// The 'error_code' field number (2) is shared by both response messages.
int EffectiveErrorCodeNumber(
  int mapped_number,
  const ::google::protobuf::UnknownFieldSet& unknown_fields);

// Maps a transport-level failure (a gRPC status) to the SDK's public
// 'ErrorCode'. 'kDeadlineExceeded' and 'kCancelled' get their own typed codes
// rather than collapsing to 'kInternalError' so a caller can tell a retryable
// timeout or cancellation apart from a genuine server bug.
api::ErrorCode TransportErrorCode(const ::grpc::Status& status);

// Formats a gRPC status as a compact, human-readable detail string for error
// messages, e.g. "grpc_status=12(UNIMPLEMENTED)" or, when the server supplied
// text, "grpc_status=13(INTERNAL): <message>". Distinct transport failures such
// as UNIMPLEMENTED map to the same public 'ErrorCode', so this keeps the
// underlying cause (like a client/server service-path or version mismatch)
// visible instead of surfacing an empty message.
std::string TransportStatusDetail(const ::grpc::Status& status);

// Delivers a recovered chunk without letting a handler exception escape.
void SafeOnChunk(api::ReadHandler& handler, const api::RecoveredChunk& chunk);

// Delivers a read failure without letting a handler exception escape.
void SafeOnError(api::ReadHandler& handler, const api::ReadError& error);

// Delivers a write acknowledgement without letting a handler exception escape.
void SafeOnSuccess(api::WriteHandler& handler, const api::WriteAck& ack);

// Delivers a write failure without letting a handler exception escape.
void SafeOnError(api::WriteHandler& handler, const api::WriteError& error);

// Emits one compact stderr line for a caught/suppressed exception in an SDK
// callback/provider path. Logging is mutex-protected and noexcept: failures
// while formatting or writing are swallowed.
void LogSuppressedException(const char *context, const char *what_message);

// Emits one 'RecoveredChunk' per range in the response. The payload is the
// concatenation of the bytes for every non-zero range in 'range_vec', in
// order; zero-fill ranges contribute nothing. Before emitting, the ranges are
// validated to stay within the requested 'region' and to account for exactly
// the payload; on a mismatch nothing is emitted and false is returned so the
// caller can report the malformed response without forwarding wrongly-sliced
// chunks. On success, payload ownership is transferred from 'value' into the
// emitted chunks (no payload copy).
bool EmitRecoveredChunks(
  const api::ReadRegion& region,
  pb::VDiskReadRet& value,
  api::ReadHandler& handler);

// Builds a 'ReadError' from a failed read response. A data-path failure
// carries exactly one failing '[offset, length)' range per response (server
// contract), so range_vec(0) is the authoritative failing extent. Setup-time
// failures carry no range, so fall back to the region's bounds.
api::ReadError ToReadError(
  const api::ReadRegion& region,
  const pb::VDiskReadRet& value,
  int error_code_number);

// Builds one write arg for a region. 'zero_data' mirrors
// 'WriteRegion::ZeroData()': a caller-declared zero-fill hole carries no
// payload, while a data region attaches its bytes.
pb::VDiskWriteArg BuildWriteArg(
  int64_t sequence_number,
  const api::WriteRegion& region,
  bool zero_data,
  api::OwnedWriteBuffer write_buffer);

// Bytes a region reserves from the write byte budget: a data region reserves
// its full length, a zero-fill region reserves nothing.
int64_t BudgetReservationFor(const api::WriteRegion& region);

// A stream-level failure captured while a stream was live: either a typed
// server refusal (with its message) or a transport status (pre-mapped to an
// 'ErrorCode'). 'Absent' means the stream ended cleanly. Used to fail every
// unreported region with the real cause.
class StreamFailure {
 public:
  // A carrier with no failure captured yet.
  StreamFailure() = default;

  // A typed server refusal: the wire 'code' and its human-readable 'message'.
  static StreamFailure Typed(api::ErrorCode code, std::string message);

  // A transport status, pre-mapped to a typed 'code'. 'message' carries the
  // transport-level detail (e.g. the raw gRPC status) so a mapped code that
  // collapses several transport failures (like UNIMPLEMENTED) is still
  // diagnosable at the call site.
  static StreamFailure Transport(api::ErrorCode code, std::string message = "");

  bool Present() const { return kind_ != Kind::kAbsent; }

  bool IsTyped() const { return kind_ == Kind::kTyped; }

  api::ErrorCode Code() const { return code_; }

  const std::string& Message() const { return message_; }

 private:
  enum class Kind { kAbsent, kTyped, kTransport };

  // Whether and how a failure was captured.
  Kind kind_ = Kind::kAbsent;

  // The typed reason (server refusal, or mapped transport status).
  api::ErrorCode code_ = api::ErrorCode::kInternalError;

  // Server-supplied message for a typed refusal; empty otherwise.
  std::string message_;
};

// Builds the terminal 'ReadError' for a region from the stream's captured
// failure. A typed refusal's message is used when present; otherwise the
// caller's contextual default is kept.
api::ReadError TerminalReadError(
  const StreamFailure& cause,
  const api::ReadRegion& region,
  const std::string& default_message);

// Builds the terminal 'WriteError' for a region that never got its own ack,
// from the stream's captured failure, mirroring 'TerminalReadError'.
api::WriteError TerminalWriteError(
  const StreamFailure& cause,
  const api::WriteRegion& region,
  const std::string& default_message);

// Returns, via 'out', the stream-level failure to raise for a write response
// that matched no pending region, or false if it should merely be ignored.
// Sequence numbers start at 1, so an unsequenced (seq == 0) response carrying
// an error is the server's connection-admission rejection.
bool StreamFailureFor(const pb::VDiskWriteRet& value, StreamFailure *out);

// A simple counting semaphore (C++17 has no 'std::counting_semaphore').
// It is intentionally non-fair (notify-one, no waiter queue).
// Used for MaxConcurrentReads and for in-flight write request count.
// Acquire blocks until a permit is free; 'Release(count)' wakes waiters.
class CountingSemaphore {
 public:
  explicit CountingSemaphore(int permits) : permits_(permits) {}

  // Blocks until a permit is available, then takes it.
  void Acquire();

  // Returns one permit and wakes a waiter.
  void Release();

  // Returns 'count' permits at once (used on stream teardown to wake waiters).
  void Release(int count);

 private:
  std::mutex mutex_;
  std::condition_variable available_;

  // Number of free permits.
  int permits_;
};

// A bytes-denominated gate that bounds the aggregate unacked write payload in
// flight, and thus peak client memory. Holds the used total as a 64-bit value
// so a budget larger than 'INT32_MAX' is supported. The sender reserves a
// region's bytes before pulling them and releases the same amount on ack (or
// when the region is dropped). 'Abort' wakes any waiter on teardown.
class ByteBudget {
 public:
  explicit ByteBudget(int64_t capacity) : capacity_(capacity) {}

  // Blocks until the budget can admit 'bytes', then reserves them. Once
  // aborted it stops waiting and reserves regardless, so the caller's matching
  // release keeps the total balanced. A reservation larger than 'capacity'
  // could never be admitted; the sender's up-front size check prevents that.
  void Acquire(int64_t bytes);

  // Returns 'bytes' to the budget and wakes any waiter.
  void Release(int64_t bytes);

  // Stops blocking new and pending acquirers; used when the stream tears down.
  void Abort();

 private:
  const int64_t capacity_;
  std::mutex mutex_;
  std::condition_variable has_room_;

  // Bytes currently reserved.
  int64_t used_ = 0;

  // Whether the budget has been aborted (stream tearing down).
  bool aborted_ = false;
};

}  // namespace internal
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_INTERNAL_GRPC_VDISK_CLIENT_SUPPORT_H_
