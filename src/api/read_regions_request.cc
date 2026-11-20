/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Validating construction for a multi-region read request.
 */

#include "nutanix/securedata/sdk/api/read_regions_request.h"

#include <stdexcept>

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

ReadRegionsRequest::ReadRegionsRequest(std::vector<ReadRegion> regions)
    : regions_(std::move(regions)) {
  if (regions_.empty()) {
    throw std::invalid_argument("regions must not be empty");
  }
}

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix
