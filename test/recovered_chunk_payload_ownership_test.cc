/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Regression test for zero-copy retained read chunk payload ownership transfer.
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "internal/grpc_vdisk_client_support.h"
#include "nutanix/securedata/sdk/api/read_error.h"
#include "nutanix/securedata/sdk/api/read_handler.h"
#include "nutanix/securedata/sdk/api/read_region.h"
#include "nutanix/securedata/sdk/api/recovered_chunk.h"

namespace {

using nutanix::securedata::sdk::api::ReadError;
using nutanix::securedata::sdk::api::ReadHandler;
using nutanix::securedata::sdk::api::ReadRegion;
using nutanix::securedata::sdk::api::RecoveredChunk;
using nutanix::securedata::sdk::internal::EmitRecoveredChunks;
namespace pb = ::nutanix::securedata::sdk::internal::pb;

class CapturingReadHandler : public ReadHandler {
 public:
  void OnChunk(const RecoveredChunk& chunk) override {
    chunks.push_back(chunk);
  }

  void OnError(const ReadError& error) override { errors.push_back(error); }

  std::vector<RecoveredChunk> chunks;
  std::vector<ReadError> errors;
};

TEST(EmitRecoveredChunksTest, RetainedChunksOwnTransferredPayloadWithoutCopy) {
  ReadRegion region(/*offset=*/100, /*length=*/6);
  pb::VDiskReadRet response;
  response.set_data("abcdef");
  pb::DiskDataRange *first = response.add_range_vec();
  first->set_offset(100);
  first->set_length(3);
  first->set_zero_data(false);
  pb::DiskDataRange *second = response.add_range_vec();
  second->set_offset(103);
  second->set_length(3);
  second->set_zero_data(false);

  const char *payload_ptr_before_emit = response.data().data();
  ASSERT_NE(payload_ptr_before_emit, nullptr);

  CapturingReadHandler handler;
  ASSERT_TRUE(EmitRecoveredChunks(region, response, handler));
  ASSERT_EQ(handler.errors.size(), 0U);
  ASSERT_EQ(handler.chunks.size(), 2U);

  // Payload ownership moved out of the protobuf response.
  EXPECT_TRUE(response.data().empty());

  // Chunks still point at the same transferred payload memory (no copy).
  EXPECT_EQ(handler.chunks[0].Data().data(), payload_ptr_before_emit);
  EXPECT_EQ(handler.chunks[1].Data().data(), payload_ptr_before_emit + 3);

  // Clearing/reusing the response must not invalidate retained chunk data.
  response.Clear();
  EXPECT_EQ(handler.chunks[0].Data(), "abc");
  EXPECT_EQ(handler.chunks[1].Data(), "def");
}

}  // namespace
