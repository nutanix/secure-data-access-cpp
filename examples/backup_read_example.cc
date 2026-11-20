/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Example: incremental-backup read driven by changed-region tracking.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nutanix/securedata/sdk/api/error_code.h"
#include "nutanix/securedata/sdk/api/read_error.h"
#include "nutanix/securedata/sdk/api/read_handler.h"
#include "nutanix/securedata/sdk/api/read_region.h"
#include "nutanix/securedata/sdk/api/recovered_chunk.h"
#include "nutanix/securedata/sdk/secure_data_access.h"

// Incremental-backup read example for the Secure Data C++ SDK.
//
// An incremental backup only copies what changed since the last recovery
// point. The application first asks the Nutanix Changed Region Tracking (CBT)
// API which regions changed -- that call classifies each region as ZEROED or
// REGULAR -- and then reads back only the REGULAR (data) regions with this
// SDK. ZEROED regions are recorded as holes in the backup and never read.
//
// The read path is streaming: a single ReadHandler exposes one callback for
// recovered chunks and one for failed ranges, each invoked as soon as it
// arrives. Every chunk and error carries its absolute vdisk offset, so a real
// backup persists each chunk directly at that offset (for example
// pwrite(fd, data, length, offset)) and retries failed ranges rather than
// accumulating the whole read in memory.

namespace {

using nutanix::securedata::sdk::SecureDataAccess;
using nutanix::securedata::sdk::api::ErrorCode;
using nutanix::securedata::sdk::api::ErrorCodeName;
using nutanix::securedata::sdk::api::ReadError;
using nutanix::securedata::sdk::api::ReadHandler;
using nutanix::securedata::sdk::api::ReadRegion;
using nutanix::securedata::sdk::api::RecoveredChunk;

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

// Returns a short hexadecimal preview so the example proves data is available
// without printing an entire vdisk chunk to stdout.
std::string Preview(const std::string_view data, size_t max_bytes) {
  if (data.empty()) {
    return "<empty>";
  }
  std::string out;
  const size_t bytes_to_show = std::min(data.size(), max_bytes);
  char buffer[4];
  for (size_t i = 0; i < bytes_to_show; ++i) {
    if (i > 0) {
      out.push_back(' ');
    }
    std::snprintf(
      buffer,
      sizeof(buffer),
      "%02x",
      static_cast<unsigned char>(data[i]));
    out.append(buffer);
  }
  if (data.size() > bytes_to_show) {
    out.append(" ...");
  }
  return out;
}

// Prints recovered data and failures. A real backup would instead persist each
// chunk at its absolute Offset() (materializing zeros when ZeroData() is true)
// and retry the [offset, length) range of each error, optionally keyed off
// Code(). 'chunk.Data()' remains valid while this 'RecoveredChunk' object
// remains alive. Retain or copy the chunk according to the application's
// ownership and asynchronous-processing needs.
class PrintingReadHandler : public ReadHandler {
 public:
  void OnChunk(const RecoveredChunk& chunk) override {
    std::cout << "READ offset=" << chunk.Offset()
              << " length=" << chunk.Length()
              << " zeroData=" << (chunk.ZeroData() ? "true" : "false")
              << " preview=" << Preview(chunk.Data(), 16) << std::endl;
  }

  void OnError(const ReadError& error) override {
    std::cerr << "READ FAILED offset=" << error.Offset()
              << " length=" << error.Length()
              << " errorCode=" << ErrorCodeName(error.Code())
              << " message=" << error.Message() << std::endl;
  }
};

}  // namespace

int main() {
  // Base range size used for the stand-in changed regions below.
  const int64_t offset = EnvOrInt("VDISK_OFFSET", 0);
  const int64_t length = EnvOrInt("VDISK_LENGTH", 1LL << 20);

  // Connection settings default to the local SSH tunnel used in development.
  const std::string host = EnvOr("VDISK_HOST", "127.0.0.1");
  const int port = static_cast<int>(EnvOrInt("VDISK_PORT", 14231));
  const std::string token = EnvOr("VDISK_TOKEN", "TOKEN");
  // The changed data regions to back up. A real backup builds this list from
  // the CBT "changed regions" response, keeping only the REGULAR (non-zero)
  // entries. Each region is read on its own stream; the SDK keeps a bounded
  // number of streams in flight and queues the rest, so this list can be
  // arbitrarily long.
  std::vector<ReadRegion> changed_regions = {
    ReadRegion(offset, length),
    ReadRegion(offset + (1LL << 30), length),
    ReadRegion(offset + (2LL << 30), length),
  };

  try {
    SecureDataAccess::Client client(host, port, token);
    PrintingReadHandler handler;
    // App code owns each chunk: write it to the backup target at
    // chunk.Offset(), checksum it, or process it inline. Failed ranges arrive
    // on the same handler's error callback so the backup can retry exactly
    // those bytes.
    client.ReadRegions(std::move(changed_regions), handler);
  } catch (const std::exception& e) {
    std::cerr << "Backup read failed: " << e.what() << std::endl;
    std::cerr << "Hint: verify VDISK_HOST, VDISK_PORT, and VDISK_TOKEN values."
              << std::endl;
    return 1;
  }
  return 0;
}
