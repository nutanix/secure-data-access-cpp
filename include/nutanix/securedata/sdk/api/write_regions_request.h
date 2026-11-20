/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * A write request of one or more regions (one active writer per vdisk).
 */

#ifndef NUTANIX_SECUREDATA_SDK_API_WRITE_REGIONS_REQUEST_H_
#define NUTANIX_SECUREDATA_SDK_API_WRITE_REGIONS_REQUEST_H_

#include <utility>
#include <vector>

#include "nutanix/securedata/sdk/api/write_region.h"

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

// A write message carrying one or more regions.
//
// Each 'WriteRegion' maps one-to-one to a proto 'DiskDataRange'. The disk to
// write to is determined by the bearer token on the connection, so the request
// carries only the regions.
//
// Single writer per vDisk. The server permits at most one active write stream
// per vDisk and rejects any second concurrent stream with
// 'ErrorCode::kConcurrentRequestNotSupported' followed by a transport cancel.
// Because the bearer token fixes the target disk, two overlapping writes to
// the same disk always collide -- whether they come from two concurrent write
// calls on one client or from two clients sharing a token. Serialize writes to
// a given disk; batch what would otherwise be concurrent writes into a single
// request's region list instead.
class WriteRegionsRequest {
 public:
  // Builds a request over 'regions'. Throws 'std::invalid_argument' if the
  // list is empty.
  explicit WriteRegionsRequest(std::vector<WriteRegion> regions);

  // The regions to write, in submission order.
  const std::vector<WriteRegion>& Regions() const { return regions_; }

 private:
  // The regions to write; never empty.
  std::vector<WriteRegion> regions_;
};

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_API_WRITE_REGIONS_REQUEST_H_
