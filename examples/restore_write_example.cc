/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Example: restore write replaying data and zero-fill regions in one call.
 */

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "nutanix/securedata/sdk/api/error_code.h"
#include "nutanix/securedata/sdk/api/write_ack.h"
#include "nutanix/securedata/sdk/api/write_data_provider.h"
#include "nutanix/securedata/sdk/api/write_error.h"
#include "nutanix/securedata/sdk/api/write_handler.h"
#include "nutanix/securedata/sdk/api/write_region.h"
#include "nutanix/securedata/sdk/secure_data_access.h"

// Restore write example for the Secure Data C++ SDK.
//
// Restore is a one-way replay: the application takes the regions captured by a
// backup and writes them to the target vdisk in a single WriteRegions call.
// Each region is a pure destination descriptor -- it says only where and how
// much to write -- and its type is declared by the caller from the backup's
// own metadata:
//   - a data region with WriteRegion::OfData: the bytes are pulled on the fly
//     from a WriteDataProvider (the backup source) one range at a time, so
//     peak memory stays bounded by the in-flight window rather than the total
//     restore size;
//   - a hole with WriteRegion::OfZeros: no bytes travel the wire and the
//     provider is never consulted. A backup marks a region a hole when CBT
//     classified it ZEROED, so the restore never has to scan for zeros.
//
// A plain contiguous restore is just a one-element region list.

namespace {

using nutanix::securedata::sdk::SecureDataAccess;
using nutanix::securedata::sdk::api::ErrorCodeName;
using nutanix::securedata::sdk::api::OwnedWriteBuffer;
using nutanix::securedata::sdk::api::WriteAck;
using nutanix::securedata::sdk::api::WriteError;
using nutanix::securedata::sdk::api::WriteHandler;
using nutanix::securedata::sdk::api::WriteRegion;

// Size of each restored data extent in this example.
constexpr int32_t kExtentBytes = 64 * 1024;

// Returns the environment variable 'name', or 'fallback' when it is unset.
std::string EnvOr(const char *name, const std::string& fallback) {
  const char *value = std::getenv(name);
  return value != nullptr ? std::string(value) : fallback;
}

// Returns the environment variable 'name' parsed as an integer, or 'fallback'.
int64_t EnvOrInt(const char *name, int64_t fallback) {
  const char *value = std::getenv(name);
  return value != nullptr ? std::stoll(value) : fallback;
}

// How a caller supplies data: the SDK calls the provider once per data region
// with the range it is about to send, so the bytes are produced on the fly and
// never held ahead of time. A real implementation would seek to 'offset' in
// the backup source and read 'length' bytes.
//
// Return exclusive std::string ownership for the fast path: the SDK transfers
// it into protobuf (set_allocated_data), avoiding an extra provider-to-proto
// copy before transport serialization.
OwnedWriteBuffer ProvideBackupData(int64_t offset, int32_t length) {
  // Deterministic non-zero pattern standing in for real restored bytes.
  std::unique_ptr<std::string> data(
    new std::string(static_cast<size_t>(length), '\0'));
  for (int32_t i = 0; i < length; ++i) {
    (*data)[i] = static_cast<char>(((offset + i) & 0xFF) | 0x01);
  }
  return OwnedWriteBuffer(std::move(data));
}

// Prints per-region acknowledgements and failures.
class PrintingWriteHandler : public WriteHandler {
 public:
  void OnSuccess(const WriteAck& ack) override {
    std::cout << "  wrote offset=" << ack.Offset() << " length=" << ack.Length()
              << std::endl;
  }

  void OnError(const WriteError& error) override {
    std::cout << "  FAILED offset=" << error.Offset()
              << " length=" << error.Length()
              << " code=" << ErrorCodeName(error.Code())
              << " message=" << error.Message() << std::endl;
  }
};

}  // namespace

int main() {
  // The regions to restore, described only by offset, length, and type. A real
  // restore builds this list from the backup's captured regions: data extents
  // become OfData, and regions the backup recorded as holes (CBT ZEROED)
  // become OfZeros.
  std::vector<WriteRegion> regions = {
    WriteRegion::OfData(1LL << 30, kExtentBytes),
    WriteRegion::OfData(2LL << 30, kExtentBytes),
    // A region the backup recorded as a hole: declared zero-fill, so no bytes
    // are sent and the source is never consulted for it.
    WriteRegion::OfZeros(3LL << 30, 4 << 20),
  };

  const std::string host = EnvOr("VDISK_HOST", "127.0.0.1");
  const int port = static_cast<int>(EnvOrInt("VDISK_PORT", 14231));
  const std::string token = EnvOr("VDISK_TOKEN", "TOKEN");
  try {
    SecureDataAccess::Client client(host, port, token);
    PrintingWriteHandler handler;
    // One data source for the whole restore; the SDK pulls each data region's
    // range from it on the fly.
    client.WriteRegions(std::move(regions), ProvideBackupData, handler);
  } catch (const std::exception& e) {
    std::cerr << "Restore write failed: " << e.what() << std::endl;
    std::cerr << "Hint: verify VDISK_HOST, VDISK_PORT, and VDISK_TOKEN values."
              << std::endl;
    return 1;
  }
  return 0;
}
