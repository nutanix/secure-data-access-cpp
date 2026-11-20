/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * One piece of recovered read data (or a zero-fill hole) at an absolute offset.
 */

#ifndef NUTANIX_SECUREDATA_SDK_API_RECOVERED_CHUNK_H_
#define NUTANIX_SECUREDATA_SDK_API_RECOVERED_CHUNK_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

// One piece of data recovered from a read, delivered to
// 'ReadHandler::OnChunk' as soon as it arrives.
//
// The chunk is self-locating: 'Offset()' is the absolute vdisk offset, so a
// caller can persist it (for example 'pwrite(fd, data, length, offset)')
// without buffering or reordering, even though chunks may arrive out of order.
// All regions in a single read belong to the same vdisk, so the absolute
// offset is sufficient to place every chunk; no region index is needed.
//
// The payload is exposed as a non-owning 'std::string_view' and is empty when
// 'ZeroData()' is true; in that case 'Length()' tells how many zeros to
// materialize.
//
// A chunk may also retain shared ownership of the backing payload, making
// 'Data()' safe to use as long as the 'RecoveredChunk' object itself remains
// alive.
class RecoveredChunk {
 public:
  RecoveredChunk(
    int64_t offset,
    int64_t length,
    std::string_view data,
    bool zero_data,
    std::shared_ptr<const std::string> payload_owner)
      : offset_(offset),
        length_(length),
        data_(data),
        zero_data_(zero_data),
        payload_owner_(std::move(payload_owner)) {}

  // Absolute vdisk offset of this chunk.
  int64_t Offset() const { return offset_; }

  // Chunk length in bytes.
  int64_t Length() const { return length_; }

  // Recovered bytes; empty when 'ZeroData()' is true.
  std::string_view Data() const { return data_; }

  // True when this chunk is a hole; materialize 'Length()' zeros.
  bool ZeroData() const { return zero_data_; }

 private:
  // Absolute vdisk offset of this chunk.
  int64_t offset_;

  // Chunk length in bytes.
  int64_t length_;

  // Recovered bytes; empty when 'zero_data_' is true.
  std::string_view data_;

  // True when this chunk is a zero-fill hole.
  bool zero_data_;

  // Optional shared owner for the backing payload bytes referenced by 'data_'.
  std::shared_ptr<const std::string> payload_owner_;
};

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_API_RECOVERED_CHUNK_H_
