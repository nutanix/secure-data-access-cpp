/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Typed error codes the SDK surfaces for read and write operations.
 */

#ifndef NUTANIX_SECUREDATA_SDK_API_ERROR_CODE_H_
#define NUTANIX_SECUREDATA_SDK_API_ERROR_CODE_H_

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

// Reason a read or write range failed, reported on 'ReadError::Code()' and
// 'WriteError::Code()'.
//
// These are the failure-only codes surfaced to SDK callers, mapped from the
// server's wire enum. Whether a given code is worth retrying is left entirely
// to the caller's policy.
enum class ErrorCode {
  // Server hit an unexpected internal condition.
  kInternalError,

  // Request was malformed (for example a bad offset, length, or payload).
  kInvalidArgument,

  // The requested disk could not be found.
  kDiskNotFound,

  // The underlying disk read failed.
  kDiskReadError,

  // The underlying disk write failed.
  kDiskWriteError,

  // The bearer token was rejected or lacks access.
  kPermissionDenied,

  // A concurrent request was rejected on a stream that allows only one.
  kConcurrentRequestNotSupported,

  // The server refused the operation because it is at capacity -- most often
  // its process-wide, shared limit on active read or write streams. This is a
  // back-off signal, not a data or region failure: the region was never
  // touched, so the whole operation should be retried after a delay (ideally
  // with exponential backoff) rather than treated as a permanent error.
  kServerBusy,

  // Server explicitly asked the client to retry.
  kRetry,

  // The optional overall transfer deadline elapsed before the range completed.
  // Distinct from a server failure -- the range's state is unknown, so it is
  // typically safe to retry.
  // NOTE: there is currently no inactivity/progress timeout. Channel keepalive
  // detects a dead connection, not a server that is alive but has stopped
  // making progress on the stream. Set 'OverallDeadline' if you need a hard
  // bound on a transfer.
  kDeadlineExceeded,

  // The RPC was cancelled before the range completed (by the caller, the
  // transport, or the peer). Like 'kDeadlineExceeded', the range's state is
  // unknown rather than known-failed.
  kCancelled,

  // The server returned a code this SDK version does not recognize. Upgrade
  // the SDK to interpret newer codes.
  kUnknown
};

// Returns a stable human-readable name for 'code', for logging and examples.
const char *ErrorCodeName(ErrorCode code);

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_API_ERROR_CODE_H_
