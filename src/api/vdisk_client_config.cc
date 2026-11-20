/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Builder validation and construction for the immutable vdisk client config.
 */

#include "nutanix/securedata/sdk/api/vdisk_client_config.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

namespace {

// Throws 'std::invalid_argument' if 'value' is negative.
void RequireNonNegative(std::chrono::milliseconds value, const char *name) {
  if (value < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument(std::string(name) + " must not be negative");
  }
}

// Keepalive is enabled only when both knobs are positive. In that enabled mode,
// timeout must be strictly less than interval.
void ValidateKeepAliveSettings(
  std::chrono::milliseconds keep_alive_time,
  std::chrono::milliseconds keep_alive_timeout) {
  RequireNonNegative(keep_alive_time, "keepAliveTime");
  RequireNonNegative(keep_alive_timeout, "keepAliveTimeout");
  if (
    keep_alive_time == std::chrono::milliseconds::zero() ||
    keep_alive_timeout == std::chrono::milliseconds::zero()) {
    return;
  }
  if (keep_alive_time.count() > std::numeric_limits<int>::max()) {
    throw std::invalid_argument(
      "keepAliveTime exceeds gRPC int-millisecond limit");
  }
  if (keep_alive_timeout.count() > std::numeric_limits<int>::max()) {
    throw std::invalid_argument(
      "keepAliveTimeout exceeds gRPC int-millisecond limit");
  }
  if (keep_alive_timeout >= keep_alive_time) {
    throw std::invalid_argument(
      "keepAliveTimeout must be strictly less than keepAliveTime when "
      "keepalive is enabled");
  }
}

}  // namespace

VDiskClientConfig::VDiskClientConfig(const Builder& builder)
    : host_(builder.host_),
      port_(builder.port_),
      keep_alive_time_(builder.keep_alive_time_),
      keep_alive_timeout_(builder.keep_alive_timeout_),
      overall_deadline_(builder.overall_deadline_),
      auth_token_(builder.auth_token_),
      max_concurrent_reads_(builder.max_concurrent_reads_),
      max_write_bytes_per_request_(builder.max_write_bytes_per_request_),
      max_in_flight_write_requests_(builder.max_in_flight_write_requests_),
      max_in_flight_write_bytes_(builder.max_in_flight_write_bytes_) {
  if (host_.empty()) {
    throw std::invalid_argument("host is required");
  }
  ValidateKeepAliveSettings(keep_alive_time_, keep_alive_timeout_);
  RequireNonNegative(overall_deadline_, "overallDeadline");
  if (max_concurrent_reads_ <= 0) {
    throw std::invalid_argument("maxConcurrentReads must be > 0");
  }
  if (max_write_bytes_per_request_ <= 0) {
    throw std::invalid_argument("maxWriteBytesPerRequest must be > 0");
  }
  if (max_in_flight_write_requests_ <= 0) {
    throw std::invalid_argument("maxInFlightWriteRequests must be > 0");
  }
  if (max_in_flight_write_bytes_ <= 0) {
    throw std::invalid_argument("maxInFlightWriteBytes must be > 0");
  }
  // A single region reserves up to maxWriteBytesPerRequest from the budget, so
  // the budget must be able to admit at least one max-size region or that
  // region could never acquire and the write would stall forever.
  if (max_in_flight_write_bytes_ < max_write_bytes_per_request_) {
    throw std::invalid_argument(
      "maxInFlightWriteBytes (" + std::to_string(max_in_flight_write_bytes_) +
      ") must be >= maxWriteBytesPerRequest (" +
      std::to_string(max_write_bytes_per_request_) + ")");
  }
}

VDiskClientConfig::Builder& VDiskClientConfig::Builder::Host(std::string host) {
  host_ = std::move(host);
  return *this;
}

VDiskClientConfig::Builder& VDiskClientConfig::Builder::Port(int port) {
  port_ = port;
  return *this;
}

VDiskClientConfig::Builder& VDiskClientConfig::Builder::KeepAliveTime(
  std::chrono::milliseconds keep_alive_time) {
  keep_alive_time_ = keep_alive_time;
  ValidateKeepAliveSettings(keep_alive_time_, keep_alive_timeout_);
  return *this;
}

VDiskClientConfig::Builder& VDiskClientConfig::Builder::KeepAliveTimeout(
  std::chrono::milliseconds keep_alive_timeout) {
  keep_alive_timeout_ = keep_alive_timeout;
  ValidateKeepAliveSettings(keep_alive_time_, keep_alive_timeout_);
  return *this;
}

VDiskClientConfig::Builder& VDiskClientConfig::Builder::OverallDeadline(
  std::chrono::milliseconds overall_deadline) {
  overall_deadline_ = overall_deadline;
  return *this;
}

VDiskClientConfig::Builder& VDiskClientConfig::Builder::AuthToken(
  std::string auth_token) {
  auth_token_ = std::move(auth_token);
  return *this;
}

VDiskClientConfig::Builder& VDiskClientConfig::Builder::MaxConcurrentReads(
  int max_concurrent_reads) {
  max_concurrent_reads_ = max_concurrent_reads;
  return *this;
}

VDiskClientConfig::Builder& VDiskClientConfig::Builder::MaxWriteBytesPerRequest(
  int64_t max_write_bytes_per_request) {
  max_write_bytes_per_request_ = max_write_bytes_per_request;
  return *this;
}

VDiskClientConfig::Builder&
VDiskClientConfig::Builder::MaxInFlightWriteRequests(
  int max_in_flight_write_requests) {
  max_in_flight_write_requests_ = max_in_flight_write_requests;
  return *this;
}

VDiskClientConfig::Builder& VDiskClientConfig::Builder::MaxInFlightWriteBytes(
  int64_t max_in_flight_write_bytes) {
  max_in_flight_write_bytes_ = max_in_flight_write_bytes;
  return *this;
}

VDiskClientConfig VDiskClientConfig::Builder::Build() const {
  return VDiskClientConfig(*this);
}

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix
