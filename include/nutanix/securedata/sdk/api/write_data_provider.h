/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Caller-supplied source that yields a data region's bytes on demand.
 */

#ifndef NUTANIX_SECUREDATA_SDK_API_WRITE_DATA_PROVIDER_H_
#define NUTANIX_SECUREDATA_SDK_API_WRITE_DATA_PROVIDER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

// Random-access source of the bytes to write. The SDK invokes the provider for
// one region's range at a time, on the fly, immediately before putting it on
// the wire—much like reading a chunk from a file. A single provider serves every
// region in a write call and maintains no per-region state.
//
// This stream-on-demand design prevents holding the entire payload in memory: the
// SDK only requests bytes when an in-flight send slot opens up, so only active
// requests materialize data in memory. Zero-fill regions carry no bytes and are
// never requested from the provider.
//
// The provider is invoked as 'provider(offset, length)' and must return a
// buffer of exactly 'length' bytes for the range '[offset, offset + length)'.
// If the returned value is wrong-sized or exceeds the per-request cap, the
// region is reported as failed.
//
// Memory Sizing & Lifetime:
// 'OwnedWriteBuffer' supports two data paths:
// 1) Exclusive std::string ownership ('owned_data'): SDK transfers ownership
//    into protobuf (set_allocated_data), avoiding the provider-to-protobuf copy.
// 2) Shared/non-owning view ('owner' + 'data'): SDK copies into protobuf
//    storage before transport serialization.
//
// gRPC transport still serializes into wire buffers, so neither path is full
// end-to-end transport zero-copy.
//
// Exception Handling:
// If the provider throws, the SDK safely contains the exception, reports that
// region as failed via 'WriteHandler::OnError' (with
// 'ErrorCode::kInternalError'), and continues processing remaining regions.
// Exceptions never propagate out of the write call.
struct OwnedWriteBuffer {
  // Exclusive owner of write bytes for zero-copy transfer into protobuf via
  // set_allocated_data(). When present, SDK moves this string into the proto
  // without a provider-to-proto copy.
  std::unique_ptr<std::string> owned_data;

  // Shared owner of the backing bytes referenced by 'Data()'. It may be null
  // only when 'Data()' points at storage with an independent lifetime.
  std::shared_ptr<const void> owner;

  // Non-owning view over the bytes to write.
  std::string_view data;

  OwnedWriteBuffer() = default;

  explicit OwnedWriteBuffer(std::unique_ptr<std::string> owned_data)
      : owned_data(std::move(owned_data)) {
    if (this->owned_data != nullptr) {
      data = *this->owned_data;
    }
  }

  OwnedWriteBuffer(std::shared_ptr<const void> owner, std::string_view data)
      : owner(std::move(owner)), data(data) {}
};

using WriteDataProvider =
  std::function<OwnedWriteBuffer(int64_t offset, int32_t length)>;

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_API_WRITE_DATA_PROVIDER_H_
