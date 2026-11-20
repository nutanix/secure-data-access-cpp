/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Validating construction for a single read region.
 */

#include "nutanix/securedata/sdk/api/read_region.h"

#include <stdexcept>

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

ReadRegion::ReadRegion(int64_t offset, int64_t length)
    : offset_(offset), length_(length) {
  if (offset < 0) {
    throw std::invalid_argument("offset must be >= 0");
  }
  if (length <= 0) {
    throw std::invalid_argument("length must be > 0");
  }
}

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix
