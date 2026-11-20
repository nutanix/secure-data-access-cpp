/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Transport-agnostic contract for reading, writing, and closing a vdisk client.
 */

#ifndef NUTANIX_SECUREDATA_SDK_API_VDISK_CLIENT_H_
#define NUTANIX_SECUREDATA_SDK_API_VDISK_CLIENT_H_

#include "nutanix/securedata/sdk/api/read_handler.h"
#include "nutanix/securedata/sdk/api/read_regions_request.h"
#include "nutanix/securedata/sdk/api/write_data_provider.h"
#include "nutanix/securedata/sdk/api/write_handler.h"
#include "nutanix/securedata/sdk/api/write_regions_request.h"

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

// The transport contract: read, write, close. The facade layer depends on this
// interface, not on the gRPC class directly. That is what makes gRPC
// swappable/hidden.
class VDiskClient {
 public:
  virtual ~VDiskClient() = default;

  // Reads each region on its own stream (the server allows only one read per
  // stream), keeping at most a configured number of streams in flight and
  // queuing the rest. Recovered data and per-range failures are delivered to
  // 'handler' as they arrive; callbacks are not serialized and may run
  // concurrently, so the handler must be thread-safe. The call returns once
  // every region has completed.
  virtual void Read(
    const ReadRegionsRequest& request,
    ReadHandler& handler) = 0;

  // Writes each region as its own message, pipelined on a single stream,
  // keeping at most a configured number of requests in flight and queuing the
  // rest. Bytes for each data region are pulled from 'data_provider' at send
  // time, one range at a time. Per-region acknowledgements and failures are
  // delivered to 'handler' as they arrive; callbacks may run concurrently, so
  // the handler must be thread-safe. The call returns once every region has
  // reported.
  virtual void Write(
    const WriteRegionsRequest& request,
    const WriteDataProvider& data_provider,
    WriteHandler& handler) = 0;

  // Cancels every active or pending transfer and rejects any new ones with
  // 'ErrorCode::kCancelled'. Idempotent and thread-safe. Does not itself tear
  // down the underlying transport; that happens when the client is destroyed.
  // Call from another thread to abort in-flight work. Do NOT call Close() or
  // destroy this client from a ReadHandler/WriteHandler callback — that is
  // API misuse; tear down only after Read()/Write() returns.
  virtual void Close() = 0;
};

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_API_VDISK_CLIENT_H_
