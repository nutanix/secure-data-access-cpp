/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * A single region to write: a data extent or a declared zero-fill hole.
 */

#ifndef NUTANIX_SECUREDATA_SDK_API_WRITE_REGION_H_
#define NUTANIX_SECUREDATA_SDK_API_WRITE_REGION_H_

#include <cstdint>

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

// A single region to write during a sparse/incremental write.
//
// A region is a pure destination descriptor: it says only where ('Offset()')
// and how much ('Length()') to write, and whether it is a zero-fill hole. It
// carries no data bytes. The bytes for data regions are pulled on the fly from
// the 'WriteDataProvider' passed alongside the regions, so a caller never has
// to hold the whole payload in memory. Use 'OfData' for real data (the
// provider supplies the bytes) or 'OfZeros' for a zero-filled hole (no bytes
// on the wire, the provider is not consulted). The SDK does not scan data
// regions for zeros: a caller that knows a region is empty -- for example from
// changed-region tracking -- must declare it a hole with 'OfZeros'. A data
// region's bytes are always sent as given, even if they happen to be all
// zeros.
//
// A region maps one-to-one to a single write request, so its data must fit in
// one request: callers should keep 'Length()' at or below the configured
// per-request cap ('VDiskClientConfig::MaxWriteBytesPerRequest'). A region
// whose data exceeds the cap is reported as a failed write rather than split.
//
// 'Length()' is a 32-bit value because that single wire range's proto
// 'DiskDataRange.length' is int32. Callers writing more than 'INT32_MAX' bytes
// must split the work into multiple regions.
class WriteRegion {
 public:
  // Region of 'length' real bytes at 'offset'; the bytes are pulled on the fly
  // from the write's 'WriteDataProvider' at send time.
  static WriteRegion OfData(int64_t offset, int32_t length);

  // Region that zero-fills 'length' bytes at 'offset' without sending zeros.
  static WriteRegion OfZeros(int64_t offset, int32_t length);

  // Absolute vdisk offset to write at.
  int64_t Offset() const { return offset_; }

  // Number of bytes to write; for data regions this is exactly how many bytes
  // the 'WriteDataProvider' must return.
  int32_t Length() const { return length_; }

  // True for a zero-fill hole (no bytes sent), false for real data.
  bool ZeroData() const { return zero_data_; }

 private:
  // Validates 'offset >= 0' and 'length > 0'; throws 'std::invalid_argument'
  // otherwise. The server maps each region to one wire range and rejects any
  // range with length <= 0, so an empty region is never valid.
  WriteRegion(int64_t offset, int32_t length, bool zero_data);

  // Absolute vdisk offset to write at.
  int64_t offset_;

  // Number of bytes to write.
  int32_t length_;

  // True for a zero-fill hole, false for real data.
  bool zero_data_;
};

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_API_WRITE_REGION_H_
