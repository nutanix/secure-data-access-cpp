/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Regression test for write reader-thread startup failure handling.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "grpc_vdisk_client_test_hooks.h"
#include "internal/grpc_vdisk_client.h"
#include "nutanix/securedata/sdk/api/error_code.h"
#include "nutanix/securedata/sdk/api/vdisk_client_config.h"
#include "nutanix/securedata/sdk/api/write_data_provider.h"
#include "nutanix/securedata/sdk/api/write_error.h"
#include "nutanix/securedata/sdk/api/write_handler.h"
#include "nutanix/securedata/sdk/api/write_region.h"
#include "nutanix/securedata/sdk/api/write_regions_request.h"

namespace {

using nutanix::securedata::sdk::api::ErrorCode;
using nutanix::securedata::sdk::api::OwnedWriteBuffer;
using nutanix::securedata::sdk::api::VDiskClientConfig;
using nutanix::securedata::sdk::api::WriteAck;
using nutanix::securedata::sdk::api::WriteError;
using nutanix::securedata::sdk::api::WriteHandler;
using nutanix::securedata::sdk::api::WriteRegion;
using nutanix::securedata::sdk::api::WriteRegionsRequest;
using nutanix::securedata::sdk::internal::ArmPauseBeforeRpcForTest;
using nutanix::securedata::sdk::internal::DisarmPauseBeforeRpcForTest;
using nutanix::securedata::sdk::internal::GrpcVDiskClient;
using nutanix::securedata::sdk::internal::IdleWaitBlockedForTest;
using nutanix::securedata::sdk::internal::PausedBeforeRpcForTest;
using nutanix::securedata::sdk::internal::SetFailReaderThreadStartForTest;

class CapturingWriteHandler : public WriteHandler {
 public:
  void OnSuccess(const WriteAck& ack) override { acks.push_back(ack); }

  void OnError(const WriteError& error) override { errors.push_back(error); }

  std::vector<WriteAck> acks;
  std::vector<WriteError> errors;
};

TEST(WriteReaderStartFailureTest, ReportsExactlyOneErrorPerRegion) {
  const VDiskClientConfig config = VDiskClientConfig::Builder()
                                     .Host("127.0.0.1")
                                     .Port(14231)
                                     .AuthToken("TOKEN")
                                     .Build();
  GrpcVDiskClient client(config);

  SetFailReaderThreadStartForTest(true);
  struct ResetFlag {
    ~ResetFlag() { SetFailReaderThreadStartForTest(false); }
  } reset_flag;

  const std::vector<WriteRegion> regions = {
    WriteRegion::OfData(0, 4),
    WriteRegion::OfData(4096, 4),
    WriteRegion::OfZeros(8192, 4)};

  WriteRegionsRequest request(regions);
  CapturingWriteHandler handler;
  const auto data_provider = [](int64_t, int32_t length) {
    auto data = std::make_shared<std::string>(static_cast<size_t>(length), 'x');
    return OwnedWriteBuffer(data, *data);
  };

  client.Write(request, data_provider, handler);

  EXPECT_TRUE(handler.acks.empty());
  ASSERT_EQ(handler.errors.size(), regions.size());
  for (size_t i = 0; i < regions.size(); ++i) {
    EXPECT_EQ(handler.errors[i].Offset(), regions[i].Offset());
    EXPECT_EQ(handler.errors[i].Length(), regions[i].Length());
    EXPECT_EQ(handler.errors[i].Code(), ErrorCode::kInternalError);
    EXPECT_NE(
      handler.errors[i].Message().find("failed to start write reader thread"),
      std::string::npos);
  }
}

template <typename Pred>
bool WaitUntil(Pred pred, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

// Destroy the client while Write() is inside the call but has not registered
// a context. The destructor must block until that caller returns.
TEST(WriteReaderStartFailureTest, DestructorWaitsBeforeWriteContext) {
  ArmPauseBeforeRpcForTest(1);
  struct PauseCleanup {
    std::thread *writer = nullptr;
    std::thread *destroyer = nullptr;
    bool joined = false;

    ~PauseCleanup() {
      DisarmPauseBeforeRpcForTest();
      if (joined) {
        return;
      }
      if (writer != nullptr && writer->joinable()) {
        writer->join();
      }
      if (destroyer != nullptr && destroyer->joinable()) {
        destroyer->join();
      }
    }

    void Join() {
      DisarmPauseBeforeRpcForTest();
      if (writer != nullptr && writer->joinable()) {
        writer->join();
      }
      if (destroyer != nullptr && destroyer->joinable()) {
        destroyer->join();
      }
      joined = true;
    }
  };

  const VDiskClientConfig config = VDiskClientConfig::Builder()
                                     .Host("127.0.0.1")
                                     .Port(9)
                                     .AuthToken("TOKEN")
                                     .Build();
  auto client = std::make_unique<GrpcVDiskClient>(config);
  CapturingWriteHandler handler;
  const std::vector<WriteRegion> regions = {WriteRegion::OfZeros(0, 4)};
  WriteRegionsRequest request(regions);
  const auto data_provider = [](int64_t, int32_t length) {
    auto data = std::make_shared<std::string>(static_cast<size_t>(length), 'x');
    return OwnedWriteBuffer(data, *data);
  };

  std::atomic<bool> destroyed{false};
  std::thread writer([&] { client->Write(request, data_provider, handler); });
  std::thread destroyer;
  PauseCleanup cleanup;
  cleanup.writer = &writer;
  cleanup.destroyer = &destroyer;

  ASSERT_TRUE(WaitUntil(
    [] { return PausedBeforeRpcForTest() > 0; },
    std::chrono::seconds(5)))
    << "Write() did not reach the pre-context pause";

  destroyer = std::thread([&] {
    client.reset();
    destroyed.store(true);
  });

  ASSERT_TRUE(WaitUntil(
    [&] { return IdleWaitBlockedForTest() > 0 || destroyed.load(); },
    std::chrono::seconds(5)))
    << "destructor neither blocked on the caller nor finished";
  EXPECT_GT(IdleWaitBlockedForTest(), 0);
  EXPECT_FALSE(destroyed.load());

  cleanup.Join();

  EXPECT_TRUE(handler.acks.empty());
  ASSERT_EQ(handler.errors.size(), 1u);
  EXPECT_EQ(handler.errors[0].Offset(), 0);
  EXPECT_EQ(handler.errors[0].Length(), 4);
  EXPECT_EQ(handler.errors[0].Code(), ErrorCode::kCancelled);
}

}  // namespace
