/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Factory that creates a VDiskClient from configuration (the transport seam).
 */

#ifndef NUTANIX_SECUREDATA_SDK_API_VDISK_CLIENTS_H_
#define NUTANIX_SECUREDATA_SDK_API_VDISK_CLIENTS_H_

#include <memory>

#include "nutanix/securedata/sdk/api/vdisk_client.h"
#include "nutanix/securedata/sdk/api/vdisk_client_config.h"

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

// A tiny factory: 'Create(config)' returns a gRPC-backed 'VDiskClient'. This
// is the only place the gRPC implementation is named, so it is the single seam
// between the clean API and the transport.
class VDiskClients {
 public:
  static std::unique_ptr<VDiskClient> Create(const VDiskClientConfig& config);

 private:
  VDiskClients() = delete;
};

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_API_VDISK_CLIENTS_H_
