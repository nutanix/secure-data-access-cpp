/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Facade implementation delegating to a transport-backed vdisk client.
 */

#include "nutanix/securedata/sdk/secure_data_access.h"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "nutanix/securedata/sdk/api/read_regions_request.h"
#include "nutanix/securedata/sdk/api/vdisk_client_config.h"
#include "nutanix/securedata/sdk/api/vdisk_clients.h"
#include "nutanix/securedata/sdk/api/write_regions_request.h"

namespace nutanix {
namespace securedata {
namespace sdk {

using api::ReadRegion;
using api::ReadRegionsRequest;
using api::VDiskClientConfig;
using api::VDiskClients;
using api::WriteRegion;
using api::WriteRegionsRequest;

SecureDataAccess::Client::Client(
  const std::string& host,
  int port,
  const std::string& token,
  std::optional<int64_t> keep_alive_time_seconds,
  std::optional<int64_t> keep_alive_timeout_seconds)
    : Client(
        host,
        port,
        token,
        std::move(keep_alive_time_seconds),
        VDiskClientConfig::kDefaultMaxConcurrentReads,
        std::move(keep_alive_timeout_seconds)) {}

SecureDataAccess::Client::Client(
  const std::string& host,
  int port,
  const std::string& token,
  std::optional<int64_t> keep_alive_time_seconds,
  int max_concurrent_reads,
  std::optional<int64_t> keep_alive_timeout_seconds) {
  if (keep_alive_time_seconds.has_value() && *keep_alive_time_seconds < 0) {
    throw std::invalid_argument("keepAliveTime must not be negative");
  }
  if (
    keep_alive_timeout_seconds.has_value() && *keep_alive_timeout_seconds < 0) {
    throw std::invalid_argument("keepAliveTimeout must not be negative");
  }
  const std::chrono::milliseconds keep_alive_time =
    keep_alive_time_seconds.has_value()
      ? std::chrono::seconds(*keep_alive_time_seconds)
      : VDiskClientConfig::kDefaultKeepAliveTime;
  const std::chrono::milliseconds keep_alive_timeout =
    keep_alive_timeout_seconds.has_value()
      ? std::chrono::seconds(*keep_alive_timeout_seconds)
      : VDiskClientConfig::kDefaultKeepAliveTimeout;
  if (
    keep_alive_time > std::chrono::milliseconds::zero() &&
    keep_alive_timeout > std::chrono::milliseconds::zero() &&
    keep_alive_timeout >= keep_alive_time) {
    throw std::invalid_argument(
      "keepAliveTimeout must be strictly less than keepAliveTime when "
      "keepalive is enabled");
  }
  const VDiskClientConfig config = VDiskClientConfig::Builder()
                                     .Host(host)
                                     .Port(port)
                                     .AuthToken(token)
                                     .KeepAliveTime(keep_alive_time)
                                     .KeepAliveTimeout(keep_alive_timeout)
                                     .MaxConcurrentReads(max_concurrent_reads)
                                     .Build();
  delegate_ = VDiskClients::Create(config);
}

SecureDataAccess::Client::~Client() { Close(); }

void SecureDataAccess::Client::ReadRegions(
  std::vector<ReadRegion>&& regions,
  api::ReadHandler& handler) {
  if (regions.empty()) {
    throw std::invalid_argument("regions must be provided");
  }
  delegate_->Read(ReadRegionsRequest(std::move(regions)), handler);
}

void SecureDataAccess::Client::WriteRegions(
  std::vector<WriteRegion>&& regions,
  const api::WriteDataProvider& data_provider,
  api::WriteHandler& handler) {
  if (regions.empty()) {
    throw std::invalid_argument("regions must be provided");
  }
  if (!data_provider) {
    throw std::invalid_argument("dataProvider is required");
  }
  delegate_->Write(
    WriteRegionsRequest(std::move(regions)),
    data_provider,
    handler);
}

void SecureDataAccess::Client::Close() {
  if (delegate_) {
    delegate_->Close();
  }
}

}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix
