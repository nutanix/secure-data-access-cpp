/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Smoke tests for wire/transport error-code mapping and forward compatibility.
 */

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "internal/grpc_vdisk_client_support.h"
#include "nutanix/securedata/sdk/api/error_code.h"
#include "vdisk.pb.h"

namespace {

namespace pb = ::nutanix::securedata::sdk::internal::pb;

using nutanix::securedata::sdk::api::ErrorCode;
using nutanix::securedata::sdk::internal::EffectiveErrorCodeNumber;
using nutanix::securedata::sdk::internal::ToErrorCode;
using nutanix::securedata::sdk::internal::TransportErrorCode;

TEST(ToErrorCodeTest, MapsEveryKnownWireValue) {
  EXPECT_EQ(ToErrorCode(pb::kNoError), ErrorCode::kUnknown);
  EXPECT_EQ(ToErrorCode(pb::kInternalError), ErrorCode::kInternalError);
  EXPECT_EQ(ToErrorCode(pb::kInvalidArgument), ErrorCode::kInvalidArgument);
  EXPECT_EQ(ToErrorCode(pb::kDiskNotFound), ErrorCode::kDiskNotFound);
  EXPECT_EQ(ToErrorCode(pb::kDiskReadError), ErrorCode::kDiskReadError);
  EXPECT_EQ(ToErrorCode(pb::kDiskWriteError), ErrorCode::kDiskWriteError);
  EXPECT_EQ(ToErrorCode(pb::kPermissionDenied), ErrorCode::kPermissionDenied);
  EXPECT_EQ(
    ToErrorCode(pb::kConcurrentRequestNotSupported),
    ErrorCode::kConcurrentRequestNotSupported);
  EXPECT_EQ(ToErrorCode(pb::kServerBusy), ErrorCode::kServerBusy);
  EXPECT_EQ(ToErrorCode(pb::kRetry), ErrorCode::kRetry);
}

TEST(ToErrorCodeTest, UnknownWireValueBecomesUnknown) {
  EXPECT_EQ(ToErrorCode(999), ErrorCode::kUnknown);
}

TEST(TransportErrorCodeTest, MapsStatusCodes) {
  EXPECT_EQ(
    TransportErrorCode(
      ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "")),
    ErrorCode::kInvalidArgument);
  EXPECT_EQ(
    TransportErrorCode(::grpc::Status(::grpc::StatusCode::NOT_FOUND, "")),
    ErrorCode::kDiskNotFound);
  EXPECT_EQ(
    TransportErrorCode(
      ::grpc::Status(::grpc::StatusCode::PERMISSION_DENIED, "")),
    ErrorCode::kPermissionDenied);
  EXPECT_EQ(
    TransportErrorCode(::grpc::Status(::grpc::StatusCode::UNAUTHENTICATED, "")),
    ErrorCode::kPermissionDenied);
  EXPECT_EQ(
    TransportErrorCode(::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "")),
    ErrorCode::kServerBusy);
  EXPECT_EQ(
    TransportErrorCode(
      ::grpc::Status(::grpc::StatusCode::RESOURCE_EXHAUSTED, "")),
    ErrorCode::kServerBusy);
  EXPECT_EQ(
    TransportErrorCode(
      ::grpc::Status(::grpc::StatusCode::DEADLINE_EXCEEDED, "")),
    ErrorCode::kDeadlineExceeded);
  EXPECT_EQ(
    TransportErrorCode(::grpc::Status(::grpc::StatusCode::CANCELLED, "")),
    ErrorCode::kCancelled);
  EXPECT_EQ(
    TransportErrorCode(::grpc::Status(::grpc::StatusCode::UNKNOWN, "")),
    ErrorCode::kInternalError);
}

TEST(EffectiveErrorCodeNumberTest, ReturnsMappedWhenNoUnknownField) {
  pb::VDiskWriteRet ret;
  ret.set_error_code(pb::kServerBusy);
  const int effective = EffectiveErrorCodeNumber(
    static_cast<int>(ret.error_code()),
    ret.unknown_fields());
  EXPECT_EQ(effective, pb::kServerBusy);
}

TEST(EffectiveErrorCodeNumberTest, RecoversNewerCodeFromUnknownField) {
  // Simulate a newer server: a proto2 closed enum value the parser does not
  // recognize lands in the unknown-field set, so error_code() reports the
  // default kNoError. The effective value must recover the raw wire number.
  pb::VDiskWriteRet ret;
  const int kFutureCode = 42;
  ret.mutable_unknown_fields()->AddVarint(
    pb::VDiskWriteRet::kErrorCodeFieldNumber,
    kFutureCode);

  EXPECT_EQ(ret.error_code(), pb::kNoError);
  const int effective = EffectiveErrorCodeNumber(
    static_cast<int>(ret.error_code()),
    ret.unknown_fields());
  EXPECT_EQ(effective, kFutureCode);
  EXPECT_EQ(ToErrorCode(effective), ErrorCode::kUnknown);
}

}  // namespace
