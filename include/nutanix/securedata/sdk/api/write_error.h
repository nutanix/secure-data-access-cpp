/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * A write failure for a specific region, with a typed error code.
 */

#ifndef NUTANIX_SECUREDATA_SDK_API_WRITE_ERROR_H_
#define NUTANIX_SECUREDATA_SDK_API_WRITE_ERROR_H_

#include <cstdint>
#include <string>
#include <utility>

#include "nutanix/securedata/sdk/api/error_code.h"

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

// A write failure for a specific region, delivered to 'WriteHandler::OnError'
// when the server rejects it, the SDK cannot send it, or the stream terminates.
//
// The failure is self-locating: 'Offset()' and 'Length()' describe the exact
// region that failed -- the coordinates the caller submitted -- so a caller
// can retry precisely that region. 'Code()' is a typed reason (see 'ErrorCode')
// for driving a retry workflow.
//
// ERROR SEMANTICS & RETRY CONTRACT:
//
// 1. DISCRETE ERRORS (e.g. kInvalidArgument, kInternalError during normal stream):
//    - The error applies ONLY to this specific region.
//    - The stream remains healthy for other regions.
//    - State Guarantee: The region was NOT written to disk.
//
// 2. TERMINAL / STREAM-FATAL ERRORS (e.g. kCancelled, kUnavailable during teardown):
//    - The error is emitted during stream drain after transport death or cancel.
//    - State Guarantee: AMBIGUOUS for in-flight regions. The server MAY have
//      committed the region before transport teardown. Retries must be IDEMPOTENT.
class WriteError {
 public:
  WriteError(
    int64_t offset,
    int64_t length,
    ErrorCode code,
    std::string message)
      : offset_(offset),
        length_(length),
        code_(code),
        message_(std::move(message)) {}

  // Absolute vdisk offset of the region that failed.
  int64_t Offset() const { return offset_; }

  // Length in bytes of the region that failed.
  int64_t Length() const { return length_; }

  // Typed reason the region failed.
  ErrorCode Code() const { return code_; }

  // Human-readable failure detail.
  const std::string& Message() const { return message_; }

 private:
  // Absolute vdisk offset of the region that failed.
  int64_t offset_;

  // Length in bytes of the region that failed.
  int64_t length_;

  // Typed reason the region failed.
  ErrorCode code_;

  // Human-readable failure detail.
  std::string message_;
};

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_API_WRITE_ERROR_H_
