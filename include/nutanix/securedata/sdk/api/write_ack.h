/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Acknowledgement that a single written region was accepted by the server.
 */

#ifndef NUTANIX_SECUREDATA_SDK_API_WRITE_ACK_H_
#define NUTANIX_SECUREDATA_SDK_API_WRITE_ACK_H_

#include <cstdint>

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

// Acknowledgement that a single region was written, delivered to
// 'WriteHandler::OnSuccess' when the server confirms it.
//
// The region is identified by its absolute 'Offset()' and 'Length()' -- the
// same coordinates the caller submitted -- not by any SDK-internal correlation
// id.
class WriteAck {
 public:
  WriteAck(int64_t offset, int64_t length) : offset_(offset), length_(length) {}

  // Absolute vdisk offset that was written.
  int64_t Offset() const { return offset_; }

  // Number of bytes written at 'Offset()'.
  int64_t Length() const { return length_; }

 private:
  // Absolute vdisk offset that was written.
  int64_t offset_;

  // Number of bytes written at 'offset_'.
  int64_t length_;
};

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_API_WRITE_ACK_H_
