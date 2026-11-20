/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Factory binding the clean API to the gRPC transport implementation.
 */

#include "nutanix/securedata/sdk/api/vdisk_clients.h"

#include <memory>

#include "internal/grpc_vdisk_client.h"

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

std::unique_ptr<VDiskClient> VDiskClients::Create(
  const VDiskClientConfig& config) {
  return std::make_unique<internal::GrpcVDiskClient>(config);
}

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix
