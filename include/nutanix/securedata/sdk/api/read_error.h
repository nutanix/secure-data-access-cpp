/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * A read failure for a specific vdisk range, with a typed error code.
 */

#ifndef NUTANIX_SECUREDATA_SDK_API_READ_ERROR_H_
#define NUTANIX_SECUREDATA_SDK_API_READ_ERROR_H_

#include <cstdint>
#include <string>
#include <utility>

#include "nutanix/securedata/sdk/api/error_code.h"

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

// A read failure for a specific vdisk range, delivered to
// 'ReadHandler::OnError' as soon as the server reports it.
//
// The failure is self-locating like a recovered chunk: 'Offset()' and
// 'Length()' describe the exact byte range the server failed to read, so a
// caller can retry precisely that range. When the server reports the failing
// range it is used verbatim; otherwise (for setup-time failures that carry no
// range) the bounds of the originating region are reported instead.
//
// 'Code()' is a typed reason (see 'ErrorCode') so a caller can drive its own
// retry workflow off it.
class ReadError {
 public:
  ReadError(int64_t offset, int64_t length, ErrorCode code, std::string message)
      : offset_(offset),
        length_(length),
        code_(code),
        message_(std::move(message)) {}

  // Absolute vdisk offset of the range that failed.
  int64_t Offset() const { return offset_; }

  // Length in bytes of the range that failed.
  int64_t Length() const { return length_; }

  // Typed reason the range failed.
  ErrorCode Code() const { return code_; }

  // Human-readable failure detail.
  const std::string& Message() const { return message_; }

 private:
  // Absolute vdisk offset of the range that failed.
  int64_t offset_;

  // Length in bytes of the range that failed.
  int64_t length_;

  // Typed reason the range failed.
  ErrorCode code_;

  // Human-readable failure detail.
  std::string message_;
};

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_API_READ_ERROR_H_
