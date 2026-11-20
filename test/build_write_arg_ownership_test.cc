/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Tests write payload ownership transfer/copy behavior.
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "internal/grpc_vdisk_client_support.h"
#include "nutanix/securedata/sdk/api/write_region.h"

namespace {

using nutanix::securedata::sdk::api::OwnedWriteBuffer;
using nutanix::securedata::sdk::api::WriteRegion;
using nutanix::securedata::sdk::internal::BuildWriteArg;
using nutanix::securedata::sdk::internal::pb::VDiskWriteArg;

TEST(BuildWriteArgTest, MovesOwnedStringIntoProtoWithoutProviderToProtoCopy) {
  std::unique_ptr<std::string> owned(new std::string("payload"));
  std::string *raw = owned.get();
  ASSERT_NE(raw, nullptr);

  VDiskWriteArg arg = BuildWriteArg(
    /*sequence_number=*/1,
    WriteRegion::OfData(/*offset=*/0, /*length=*/7),
    /*zero_data=*/false,
    OwnedWriteBuffer(std::move(owned)));

  // Ownership transferred into the proto string field.
  std::string *released = arg.release_data();
  ASSERT_NE(released, nullptr);
  EXPECT_EQ(released, raw);
  EXPECT_EQ(*released, "payload");
  delete released;
}

TEST(BuildWriteArgTest, CopiesWhenOnlyViewOwnershipIsProvided) {
  auto shared = std::make_shared<std::string>("payload");
  const char *raw_data = shared->data();
  ASSERT_NE(raw_data, nullptr);

  VDiskWriteArg arg = BuildWriteArg(
    /*sequence_number=*/2,
    WriteRegion::OfData(/*offset=*/0, /*length=*/7),
    /*zero_data=*/false,
    OwnedWriteBuffer(shared, *shared));

  // Shared/non-owning path copies into protobuf-owned storage.
  EXPECT_EQ(arg.data(), "payload");
  EXPECT_NE(arg.data().data(), raw_data);
}

}  // namespace
