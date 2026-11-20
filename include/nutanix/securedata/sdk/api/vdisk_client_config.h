/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Immutable connection and tuning configuration for a vdisk client.
 */

#ifndef NUTANIX_SECUREDATA_SDK_API_VDISK_CLIENT_CONFIG_H_
#define NUTANIX_SECUREDATA_SDK_API_VDISK_CLIENT_CONFIG_H_

#include <chrono>
#include <cstdint>
#include <string>

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

// Connection settings for a vdisk client.
//
// The transport always uses TLS with server-certificate verification skipped:
// authorization is established by the bearer 'AuthToken()', not by the server
// certificate, so no CA material or hostname override is needed.
class VDiskClientConfig {
 public:
  // Default per-Read() cap on streams kept in flight within one call. The
  // server's active-read-stream limit (default 64) is process-wide and shared
  // across every client, token, and vDisk. This default is set well below that
  // so one call does not consume the whole instance's read-stream capacity and
  // starve other concurrent backup or restore jobs. Concurrent Read() calls
  // each get their own cap. Raise it only when the server is largely dedicated
  // to this workload.
  static constexpr int kDefaultMaxConcurrentReads = 8;

  // Default cap on the payload bytes of a single write request. It matches the
  // server's per-stream in-flight-write-bytes default, above which the server
  // rejects a single request with 'kInvalidArgument'.
  static constexpr int64_t kDefaultMaxWriteBytesPerRequest = 30LL * 1024 * 1024;

  // Default cap on write requests kept in flight (sent but not yet acked) on a
  // write stream. Additional regions are queued and sent as earlier ones are
  // acked.
  static constexpr int kDefaultMaxInFlightWriteRequests = 20;

  // Default cap on the aggregate payload bytes kept in flight (sent but not
  // yet acked) on a write stream. This bounds peak client memory independently
  // of the request count: without it, the nominal window would be
  // request-count times per-request-cap. The sender blocks until the budget
  // has room, so large regions throttle by bytes rather than only by count.
  static constexpr int64_t kDefaultMaxInFlightWriteBytes = 30LL * 1024 * 1024;

  // Lets shorten the default timeout based on industry standards.

  // Default keepalive ping interval: disabled (0). Set a positive value to
  // enable HTTP/2 keepalive pings while a read or write call is active.
  static constexpr std::chrono::milliseconds kDefaultKeepAliveTime{
    std::chrono::seconds(0)};

  // Default keepalive ping timeout: disabled (0). Used only when keepalive
  // time is also positive.
  static constexpr std::chrono::milliseconds kDefaultKeepAliveTimeout{
    std::chrono::seconds(0)};

  // Default overall transfer deadline: zero, meaning unbounded. No single
  // wall-clock budget fits every transfer, so liveness is enforced by
  // keepalive instead. Set a positive value only to impose a hard cap on the
  // whole operation.
  static constexpr std::chrono::milliseconds kDefaultOverallDeadline{0};

  // Default gRPC port for the vdisk data service.
  static constexpr int kDefaultPort = 14231;

  class Builder;

  const std::string& Host() const { return host_; }

  int Port() const { return port_; }

  // Keepalive ping interval: how often the transport pings the server while a
  // call is active to detect a stalled or dead connection. If either
  // 'KeepAliveTime()' or 'KeepAliveTimeout()' is zero, keepalive pings are
  // disabled entirely.
  std::chrono::milliseconds KeepAliveTime() const { return keep_alive_time_; }

  // Keepalive ping timeout: how long to wait for a ping ack before treating
  // the connection as dead. If either keepalive knob is zero, pings are
  // disabled. When enabled (both positive), timeout must be strictly less than
  // interval ('KeepAliveTimeout() < KeepAliveTime()').
  std::chrono::milliseconds KeepAliveTimeout() const {
    return keep_alive_timeout_;
  }

  // Optional overall transfer deadline applied as the gRPC call deadline. Zero
  // (the default) means unbounded, so the transfer is governed only by
  // keepalive liveness.
  std::chrono::milliseconds OverallDeadline() const {
    return overall_deadline_;
  }

  // Bearer token sent as the "authorization" metadata header.
  const std::string& AuthToken() const { return auth_token_; }

  // Maximum number of read streams kept in flight within one Read() call.
  // Extra regions are queued and started as in-flight reads complete, so a
  // read call accepts an unbounded region list. Concurrent Read() calls apply
  // this cap independently (this is not a client-wide global ceiling).
  int MaxConcurrentReads() const { return max_concurrent_reads_; }

  // Maximum payload bytes allowed in a single write request. A region whose
  // data exceeds this cannot be split, so the SDK rejects such a write before
  // sending.
  int64_t MaxWriteBytesPerRequest() const {
    return max_write_bytes_per_request_;
  }

  // Maximum write requests the SDK keeps in flight (sent but not yet acked) on
  // a write stream. Additional regions are queued and sent as earlier ones are
  // acknowledged.
  int MaxInFlightWriteRequests() const { return max_in_flight_write_requests_; }

  // Maximum aggregate payload bytes the SDK keeps in flight (sent but not yet
  // acked) on a write stream. Bounds peak client memory regardless of
  // individual region sizes. Must be >= 'MaxWriteBytesPerRequest()'.
  int64_t MaxInFlightWriteBytes() const { return max_in_flight_write_bytes_; }

 private:
  friend class Builder;

  // Validates and freezes the settings from 'builder'; throws
  // 'std::invalid_argument' on any invariant violation.
  explicit VDiskClientConfig(const Builder& builder);

  // CVM host or DNS name reachable from the client.
  std::string host_;

  // gRPC port for the vdisk data service.
  int port_;

  // Keepalive ping interval; if either keepalive knob is zero, keepalive is
  // disabled.
  std::chrono::milliseconds keep_alive_time_;

  // Keepalive ping timeout; when enabled, must be < 'keep_alive_time_'.
  std::chrono::milliseconds keep_alive_timeout_;

  // Optional overall transfer deadline; zero means unbounded.
  std::chrono::milliseconds overall_deadline_;

  // Bearer token identifying the caller and the target disk.
  std::string auth_token_;

  // Maximum number of parallel region read streams allowed per Read() call.
  // Read-path memory scales with this concurrency cap. Each read stream allows
  // up to 128 MiB inbound gRPC messages.
  int max_concurrent_reads_;

  // Cap on payload bytes in a single write request.
  int64_t max_write_bytes_per_request_;

  // Cap on in-flight (unacked) write requests per stream.
  int max_in_flight_write_requests_;

  // Maximum total bytes of write payloads allowed in-flight across all active requests.
  // Because gRPC retains a serialized wire buffer alongside
  // the active Protobuf message, peak memory usage during writes is roughly ~2x the configured
  // byte limit. Callers should size this value to roughly half of their desired RAM allocation for writes.
  int64_t max_in_flight_write_bytes_;
};

// Fluent builder for 'VDiskClientConfig'. All setters return '*this' so calls
// can be chained; 'Build()' validates the accumulated settings.
class VDiskClientConfig::Builder {
 public:
  Builder& Host(std::string host);

  Builder& Port(int port);

  // Sets the keepalive ping interval. Must not be negative. If either
  // keepalive knob is set to zero, keepalive is disabled. When both are
  // positive, timeout must be strictly less than interval.
  Builder& KeepAliveTime(std::chrono::milliseconds keep_alive_time);

  // Sets the keepalive ping timeout. Must not be negative. If either keepalive
  // knob is set to zero, keepalive is disabled. When both are positive,
  // timeout must be strictly less than interval.
  Builder& KeepAliveTimeout(std::chrono::milliseconds keep_alive_timeout);

  // Sets the optional overall transfer deadline. Zero (the default) means
  // unbounded. Must not be negative.
  Builder& OverallDeadline(std::chrono::milliseconds overall_deadline);

  Builder& AuthToken(std::string auth_token);

  // Sets the per-Read() cap on in-flight read streams (must be > 0).
  Builder& MaxConcurrentReads(int max_concurrent_reads);

  // Sets the cap on payload bytes in a single write request (must be > 0).
  Builder& MaxWriteBytesPerRequest(int64_t max_write_bytes_per_request);

  // Sets the cap on in-flight (unacked) write requests per stream (must be
  // > 0).
  Builder& MaxInFlightWriteRequests(int max_in_flight_write_requests);

  // Sets the cap on aggregate in-flight (unacked) write payload bytes (must be
  // > 0 and >= 'MaxWriteBytesPerRequest').
  Builder& MaxInFlightWriteBytes(int64_t max_in_flight_write_bytes);

  // Validates the accumulated settings and returns the immutable config.
  // Throws 'std::invalid_argument' on any invariant violation.
  VDiskClientConfig Build() const;

 private:
  friend class VDiskClientConfig;

  std::string host_;
  int port_ = kDefaultPort;
  std::chrono::milliseconds keep_alive_time_ = kDefaultKeepAliveTime;
  std::chrono::milliseconds keep_alive_timeout_ = kDefaultKeepAliveTimeout;
  std::chrono::milliseconds overall_deadline_ = kDefaultOverallDeadline;
  std::string auth_token_;
  int max_concurrent_reads_ = kDefaultMaxConcurrentReads;
  int64_t max_write_bytes_per_request_ = kDefaultMaxWriteBytesPerRequest;
  int max_in_flight_write_requests_ = kDefaultMaxInFlightWriteRequests;
  int64_t max_in_flight_write_bytes_ = kDefaultMaxInFlightWriteBytes;
};

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_API_VDISK_CLIENT_CONFIG_H_
