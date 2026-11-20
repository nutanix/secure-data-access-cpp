/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Human-readable names for the SDK's typed error codes.
 */

#include "nutanix/securedata/sdk/api/error_code.h"

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

const char *ErrorCodeName(ErrorCode code) {
  switch (code) {
    case ErrorCode::kInternalError:
      return "INTERNAL_ERROR";
    case ErrorCode::kInvalidArgument:
      return "INVALID_ARGUMENT";
    case ErrorCode::kDiskNotFound:
      return "DISK_NOT_FOUND";
    case ErrorCode::kDiskReadError:
      return "DISK_READ_ERROR";
    case ErrorCode::kDiskWriteError:
      return "DISK_WRITE_ERROR";
    case ErrorCode::kPermissionDenied:
      return "PERMISSION_DENIED";
    case ErrorCode::kConcurrentRequestNotSupported:
      return "CONCURRENT_REQUEST_NOT_SUPPORTED";
    case ErrorCode::kServerBusy:
      return "SERVER_BUSY";
    case ErrorCode::kRetry:
      return "RETRY";
    case ErrorCode::kDeadlineExceeded:
      return "DEADLINE_EXCEEDED";
    case ErrorCode::kCancelled:
      return "CANCELLED";
    case ErrorCode::kUnknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix
