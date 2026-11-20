/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Smoke tests for region/request validation and client-config invariants.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <stdexcept>
#include <vector>

#include "nutanix/securedata/sdk/api/read_region.h"
#include "nutanix/securedata/sdk/api/read_regions_request.h"
#include "nutanix/securedata/sdk/api/vdisk_client_config.h"
#include "nutanix/securedata/sdk/api/write_region.h"
#include "nutanix/securedata/sdk/api/write_regions_request.h"

namespace {

using nutanix::securedata::sdk::api::ReadRegion;
using nutanix::securedata::sdk::api::ReadRegionsRequest;
using nutanix::securedata::sdk::api::VDiskClientConfig;
using nutanix::securedata::sdk::api::WriteRegion;
using nutanix::securedata::sdk::api::WriteRegionsRequest;

TEST(ReadRegionTest, AcceptsValidBounds) {
  ReadRegion region(1024, 4096);
  EXPECT_EQ(region.Offset(), 1024);
  EXPECT_EQ(region.Length(), 4096);
}

TEST(ReadRegionTest, RejectsNegativeOffset) {
  EXPECT_THROW(ReadRegion(-1, 4096), std::invalid_argument);
}

TEST(ReadRegionTest, RejectsNonPositiveLength) {
  EXPECT_THROW(ReadRegion(0, 0), std::invalid_argument);
  EXPECT_THROW(ReadRegion(0, -5), std::invalid_argument);
}

TEST(WriteRegionTest, DataAndZeroFactories) {
  WriteRegion data = WriteRegion::OfData(1 << 20, 512);
  EXPECT_EQ(data.Offset(), 1 << 20);
  EXPECT_EQ(data.Length(), 512);
  EXPECT_FALSE(data.ZeroData());

  WriteRegion hole = WriteRegion::OfZeros(1 << 21, 4096);
  EXPECT_TRUE(hole.ZeroData());
}

TEST(WriteRegionTest, RejectsInvalidBounds) {
  EXPECT_THROW(WriteRegion::OfData(-1, 512), std::invalid_argument);
  EXPECT_THROW(WriteRegion::OfData(0, 0), std::invalid_argument);
  EXPECT_THROW(WriteRegion::OfZeros(0, -1), std::invalid_argument);
}

TEST(RequestTest, RejectsEmptyRegionLists) {
  EXPECT_THROW(
    ReadRegionsRequest(std::vector<ReadRegion>{}),
    std::invalid_argument);
  EXPECT_THROW(
    WriteRegionsRequest(std::vector<WriteRegion>{}),
    std::invalid_argument);
}

TEST(RequestTest, PreservesRegionOrder) {
  ReadRegionsRequest request(
    std::vector<ReadRegion>{ReadRegion(0, 10), ReadRegion(100, 20)});
  ASSERT_EQ(request.Regions().size(), 2u);
  EXPECT_EQ(request.Regions()[0].Offset(), 0);
  EXPECT_EQ(request.Regions()[1].Offset(), 100);
}

TEST(RequestTest, AcceptsReadRegionsFromLvalueAndRvalue) {
  std::vector<ReadRegion> lvalue_regions = {
    ReadRegion(0, 10),
    ReadRegion(100, 20)};
  ReadRegionsRequest from_lvalue(lvalue_regions);
  ASSERT_EQ(from_lvalue.Regions().size(), 2u);
  EXPECT_EQ(from_lvalue.Regions()[0].Offset(), 0);
  EXPECT_EQ(from_lvalue.Regions()[1].Offset(), 100);

  std::vector<ReadRegion> rvalue_regions = {
    ReadRegion(200, 30),
    ReadRegion(400, 40)};
  ReadRegionsRequest from_rvalue(std::move(rvalue_regions));
  ASSERT_EQ(from_rvalue.Regions().size(), 2u);
  EXPECT_EQ(from_rvalue.Regions()[0].Offset(), 200);
  EXPECT_EQ(from_rvalue.Regions()[1].Offset(), 400);
}

TEST(RequestTest, AcceptsWriteRegionsFromLvalueAndRvalue) {
  std::vector<WriteRegion> lvalue_regions = {
    WriteRegion::OfData(0, 10),
    WriteRegion::OfZeros(100, 20)};
  WriteRegionsRequest from_lvalue(lvalue_regions);
  ASSERT_EQ(from_lvalue.Regions().size(), 2u);
  EXPECT_EQ(from_lvalue.Regions()[0].Offset(), 0);
  EXPECT_EQ(from_lvalue.Regions()[1].Offset(), 100);

  std::vector<WriteRegion> rvalue_regions = {
    WriteRegion::OfData(200, 30),
    WriteRegion::OfZeros(400, 40)};
  WriteRegionsRequest from_rvalue(std::move(rvalue_regions));
  ASSERT_EQ(from_rvalue.Regions().size(), 2u);
  EXPECT_EQ(from_rvalue.Regions()[0].Offset(), 200);
  EXPECT_EQ(from_rvalue.Regions()[1].Offset(), 400);
}

TEST(VDiskClientConfigTest, AppliesDefaults) {
  VDiskClientConfig config =
    VDiskClientConfig::Builder().Host("cvm").AuthToken("token").Build();
  EXPECT_EQ(config.Host(), "cvm");
  EXPECT_EQ(config.Port(), VDiskClientConfig::kDefaultPort);
  EXPECT_EQ(
    config.MaxConcurrentReads(),
    VDiskClientConfig::kDefaultMaxConcurrentReads);
  EXPECT_EQ(
    config.MaxWriteBytesPerRequest(),
    VDiskClientConfig::kDefaultMaxWriteBytesPerRequest);
  EXPECT_EQ(config.KeepAliveTime(), VDiskClientConfig::kDefaultKeepAliveTime);
  EXPECT_EQ(
    config.KeepAliveTimeout(),
    VDiskClientConfig::kDefaultKeepAliveTimeout);
  EXPECT_EQ(
    config.OverallDeadline(),
    VDiskClientConfig::kDefaultOverallDeadline);
}

TEST(VDiskClientConfigTest, KeepAliveTimeOverrideKeepsDefaultTimeout) {
  const std::chrono::seconds keep_alive_time{120};
  VDiskClientConfig config = VDiskClientConfig::Builder()
                               .Host("cvm")
                               .AuthToken("token")
                               .KeepAliveTime(keep_alive_time)
                               .Build();

  EXPECT_EQ(config.KeepAliveTime(), keep_alive_time);
  EXPECT_EQ(
    config.KeepAliveTimeout(),
    VDiskClientConfig::kDefaultKeepAliveTimeout);
}

TEST(VDiskClientConfigTest, RequiresHost) {
  EXPECT_THROW(VDiskClientConfig::Builder().Build(), std::invalid_argument);
}

TEST(VDiskClientConfigTest, RejectsNonPositiveCaps) {
  EXPECT_THROW(
    VDiskClientConfig::Builder().Host("cvm").MaxConcurrentReads(0).Build(),
    std::invalid_argument);
  EXPECT_THROW(
    VDiskClientConfig::Builder()
      .Host("cvm")
      .MaxInFlightWriteRequests(-1)
      .Build(),
    std::invalid_argument);
}

TEST(VDiskClientConfigTest, RejectsNegativeDurations) {
  EXPECT_THROW(
    VDiskClientConfig::Builder()
      .Host("cvm")
      .KeepAliveTime(std::chrono::milliseconds(-1))
      .Build(),
    std::invalid_argument);
  EXPECT_THROW(
    VDiskClientConfig::Builder()
      .Host("cvm")
      .KeepAliveTimeout(std::chrono::milliseconds(-1))
      .Build(),
    std::invalid_argument);
}

TEST(VDiskClientConfigTest, RejectsKeepAliveTimeoutAtOrAboveInterval) {
  EXPECT_THROW(
    VDiskClientConfig::Builder()
      .Host("cvm")
      .KeepAliveTime(std::chrono::seconds(5))
      .KeepAliveTimeout(std::chrono::seconds(5))
      .Build(),
    std::invalid_argument);
  EXPECT_THROW(
    VDiskClientConfig::Builder()
      .Host("cvm")
      .KeepAliveTime(std::chrono::seconds(5))
      .KeepAliveTimeout(std::chrono::seconds(6))
      .Build(),
    std::invalid_argument);
}

TEST(VDiskClientConfigTest, ZeroKeepAliveKnobsDisableKeepAliveWithoutThrowing) {
  VDiskClientConfig time_zero =
    VDiskClientConfig::Builder()
      .Host("cvm")
      .AuthToken("token")
      .KeepAliveTime(std::chrono::milliseconds::zero())
      .KeepAliveTimeout(std::chrono::seconds(20))
      .Build();
  EXPECT_EQ(time_zero.KeepAliveTime(), std::chrono::milliseconds::zero());
  EXPECT_EQ(time_zero.KeepAliveTimeout(), std::chrono::seconds(20));

  VDiskClientConfig timeout_zero =
    VDiskClientConfig::Builder()
      .Host("cvm")
      .AuthToken("token")
      .KeepAliveTime(std::chrono::seconds(30))
      .KeepAliveTimeout(std::chrono::milliseconds::zero())
      .Build();
  EXPECT_EQ(timeout_zero.KeepAliveTime(), std::chrono::seconds(30));
  EXPECT_EQ(timeout_zero.KeepAliveTimeout(), std::chrono::milliseconds::zero());
}

TEST(VDiskClientConfigTest, AcceptsExplicitKeepAliveDurations) {
  VDiskClientConfig config = VDiskClientConfig::Builder()
                               .Host("cvm")
                               .AuthToken("token")
                               .KeepAliveTime(std::chrono::seconds(30))
                               .KeepAliveTimeout(std::chrono::seconds(10))
                               .Build();
  EXPECT_EQ(config.KeepAliveTime(), std::chrono::seconds(30));
  EXPECT_EQ(config.KeepAliveTimeout(), std::chrono::seconds(10));
}

TEST(VDiskClientConfigTest, RejectsKeepAliveDurationsAboveGrpcIntMsLimit) {
  const std::chrono::milliseconds over_limit{
    static_cast<int64_t>(std::numeric_limits<int>::max()) + 1};
  EXPECT_THROW(
    VDiskClientConfig::Builder()
      .Host("cvm")
      .AuthToken("token")
      .KeepAliveTime(over_limit)
      .KeepAliveTimeout(std::chrono::seconds(10))
      .Build(),
    std::invalid_argument);
  EXPECT_THROW(
    VDiskClientConfig::Builder()
      .Host("cvm")
      .AuthToken("token")
      .KeepAliveTime(std::chrono::seconds(30))
      .KeepAliveTimeout(over_limit)
      .Build(),
    std::invalid_argument);
}

TEST(VDiskClientConfigTest, RejectsByteBudgetBelowPerRequestCap) {
  EXPECT_THROW(
    VDiskClientConfig::Builder()
      .Host("cvm")
      .MaxWriteBytesPerRequest(1024)
      .MaxInFlightWriteBytes(512)
      .Build(),
    std::invalid_argument);
}

TEST(VDiskClientConfigTest, AcceptsByteBudgetEqualToPerRequestCap) {
  VDiskClientConfig config = VDiskClientConfig::Builder()
                               .Host("cvm")
                               .MaxWriteBytesPerRequest(1024)
                               .MaxInFlightWriteBytes(1024)
                               .Build();
  EXPECT_EQ(config.MaxInFlightWriteBytes(), 1024);
}

}  // namespace
