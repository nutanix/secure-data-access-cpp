/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Validating construction and factories for a single write region.
 */

#include "nutanix/securedata/sdk/api/write_region.h"

#include <stdexcept>

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

WriteRegion::WriteRegion(int64_t offset, int32_t length, bool zero_data)
    : offset_(offset), length_(length), zero_data_(zero_data) {
  if (offset < 0) {
    throw std::invalid_argument("offset must be >= 0");
  }
  // The server maps each region to one wire range and rejects any range with
  // length <= 0, so an empty region is never valid.
  if (length <= 0) {
    throw std::invalid_argument("length must be > 0");
  }
}

WriteRegion WriteRegion::OfData(int64_t offset, int32_t length) {
  return WriteRegion(offset, length, false /* zero_data */);
}

WriteRegion WriteRegion::OfZeros(int64_t offset, int32_t length) {
  return WriteRegion(offset, length, true /* zero_data */);
}

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix
