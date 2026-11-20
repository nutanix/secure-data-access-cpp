/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * A single vdisk region to read, given by absolute offset and length.
 */

#ifndef NUTANIX_SECUREDATA_SDK_API_READ_REGION_H_
#define NUTANIX_SECUREDATA_SDK_API_READ_REGION_H_

#include <cstdint>

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

// A single vdisk region the caller wants to read.
//
// Each region maps one-to-one to its own read stream on the wire (the server
// accepts only one read per stream), so a multi-region read fans out to one
// stream per region.
class ReadRegion {
 public:
  // Builds a region starting at absolute 'offset' spanning 'length' bytes.
  // Throws 'std::invalid_argument' if 'offset' is negative or 'length' is not
  // positive.
  ReadRegion(int64_t offset, int64_t length);

  // Absolute vdisk offset to start reading from.
  int64_t Offset() const { return offset_; }

  // Number of bytes to read from 'Offset()'.
  int64_t Length() const { return length_; }

 private:
  // Absolute vdisk offset to start reading from.
  int64_t offset_;

  // Number of bytes to read from 'offset_'.
  int64_t length_;
};

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_API_READ_REGION_H_
