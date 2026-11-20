/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Thin facade hiding the gRPC transport behind a simple vdisk client.
 */

#ifndef NUTANIX_SECUREDATA_SDK_SECURE_DATA_ACCESS_H_
#define NUTANIX_SECUREDATA_SDK_SECURE_DATA_ACCESS_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nutanix/securedata/sdk/api/read_handler.h"
#include "nutanix/securedata/sdk/api/read_region.h"
#include "nutanix/securedata/sdk/api/vdisk_client.h"
#include "nutanix/securedata/sdk/api/write_data_provider.h"
#include "nutanix/securedata/sdk/api/write_handler.h"
#include "nutanix/securedata/sdk/api/write_region.h"

namespace nutanix {
namespace securedata {
namespace sdk {

// Thin facade intended for SDK consumers who should not need to know that
// transport is implemented via gRPC under the hood.
class SecureDataAccess {
 public:
  SecureDataAccess() = delete;

  // A connection to one vdisk. Owns the transport and closes it on
  // destruction (RAII). Not copyable.
  class Client {
   public:
    // Connects to a vdisk using a bearer token. The transport always uses TLS
    // with server-certificate verification skipped; authorization comes from
    // 'token', which also identifies the disk to read from and write to, so
    // the SDK holds no disk details of its own.
    //
    // Keepalive knobs detect a stalled or dead connection, not a whole-
    // operation deadline: a bulk read or restore may run for as long as the
    // connection stays alive. For finer control build a 'VDiskClientConfig' and
    // use 'VDiskClients' directly.
    //
    // Either keepalive value may be omitted:
    // - if only keep_alive_time_seconds is set, timeout uses SDK default
    // - if only keep_alive_timeout_seconds is set, interval uses SDK default
    // - if both are omitted, both SDK defaults are used
    // - if either resolved value is zero, keepalive is disabled
    //
    // When both resolved values are positive, timeout must be strictly smaller
    // than interval.
    Client(
      const std::string& host,
      int port,
      const std::string& token,
      std::optional<int64_t> keep_alive_time_seconds = std::nullopt,
      std::optional<int64_t> keep_alive_timeout_seconds = std::nullopt);

    // Same as above but also caps how many read streams one ReadRegions() call
    // keeps in flight at once. Extra regions are queued and started as earlier
    // reads finish, so the cap bounds per-call concurrency without limiting
    // the number of regions per call. Concurrent ReadRegions() calls apply
    // this cap independently.
    Client(
      const std::string& host,
      int port,
      const std::string& token,
      std::optional<int64_t> keep_alive_time_seconds,
      int max_concurrent_reads,
      std::optional<int64_t> keep_alive_timeout_seconds = std::nullopt);

    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Reads one or more, possibly non-contiguous, regions in one call. A plain
    // contiguous read is just a one-element list; an incremental-backup read
    // passes the changed extents as multiple regions. Each region is fetched
    // on its own stream; the SDK keeps a bounded number of streams in flight
    // and queues the rest, so the region list can be arbitrarily long.
    //
    // Results stream through 'handler': 'OnChunk' receives each recovered
    // chunk as it arrives and 'OnError' receives each failed range. Callbacks
    // are not serialized and may run concurrently on independent threads, so
    // the handler must be thread-safe. Do not Close() or destroy this Client
    // from a handler callback (API misuse); tear down after ReadRegions
    // returns. The call returns once every region has completed. Throws
    // 'std::invalid_argument' if 'regions' is empty.
    void ReadRegions(
      std::vector<api::ReadRegion>&& regions,
      api::ReadHandler& handler);

    // Writes one or more, possibly non-contiguous, regions in one call. A
    // region carries no bytes -- it only says where and how much to write.
    // Build one with 'WriteRegion::OfData' for real data or
    // 'WriteRegion::OfZeros' for a zero-fill hole. The bytes for data regions
    // are pulled from 'data_provider' on the fly at send time, so the caller
    // never holds the whole payload in memory.
    //
    // Results stream through 'handler': 'OnSuccess' receives each acknowledged
    // region and 'OnError' receives each failed region, both keyed by absolute
    // offset and length. Callbacks may run concurrently, so the handler must
    // be thread-safe. Do not Close() or destroy this Client from a handler
    // callback (API misuse); tear down after WriteRegions returns. The call
    // returns once every region has reported.
    //
    // Single writer per vDisk: the server allows only one active write stream
    // per vDisk and rejects a second with
    // 'ErrorCode::kConcurrentRequestNotSupported'. Serialize writes to a disk
    // and put what would be concurrent writes into a single call's region list
    // instead. Throws 'std::invalid_argument' if 'regions' is empty.
    void WriteRegions(
      std::vector<api::WriteRegion>&& regions,
      const api::WriteDataProvider& data_provider,
      api::WriteHandler& handler);

    // Cancels in-flight work. Idempotent; also called by the destructor.
    // Call from another thread to abort. Do not call from OnError/OnChunk/
    // OnSuccess — that is API misuse; let the Client close when destroyed
    // after ReadRegions/WriteRegions returns.
    void Close();

   private:
    // The transport-backed client the facade delegates to.
    std::unique_ptr<api::VDiskClient> delegate_;
  };
};

}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_SECURE_DATA_ACCESS_H_
