/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * A request to read one or more, possibly non-contiguous, vdisk regions.
 */

#ifndef NUTANIX_SECUREDATA_SDK_API_READ_REGIONS_REQUEST_H_
#define NUTANIX_SECUREDATA_SDK_API_READ_REGIONS_REQUEST_H_

#include <utility>
#include <vector>

#include "nutanix/securedata/sdk/api/read_region.h"

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

// A request to read one or more, possibly non-contiguous, vdisk regions.
//
// Every 'ReadRegion' is fetched on its own read stream (the server supports
// only one read per stream). The disk to read from is determined by the bearer
// token on the connection, so the request carries only the regions. Chunks are
// delivered to the caller as they arrive.
class ReadRegionsRequest {
 public:
  // Builds a request over 'regions'. Throws 'std::invalid_argument' if the
  // list is empty.
  explicit ReadRegionsRequest(std::vector<ReadRegion> regions);

  // The regions to read, in submission order.
  const std::vector<ReadRegion>& Regions() const { return regions_; }

 private:
  // The regions to read; never empty.
  std::vector<ReadRegion> regions_;
};

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_API_READ_REGIONS_REQUEST_H_
