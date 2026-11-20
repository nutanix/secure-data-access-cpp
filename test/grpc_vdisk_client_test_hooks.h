/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Test-only controls for the gRPC client. Linked into the smoke tests, not
 * the SDK library. grpc_vdisk_client_test_hooks.cc installs these into the
 * client's hook seam.
 */

#ifndef NUTANIX_SECUREDATA_SDK_TEST_GRPC_VDISK_CLIENT_TEST_HOOKS_H_
#define NUTANIX_SECUREDATA_SDK_TEST_GRPC_VDISK_CLIENT_TEST_HOOKS_H_

namespace nutanix {
namespace securedata {
namespace sdk {
namespace internal {

// Make the Nth WaitIfPauseBeforeRpcArmed() call block (1-based).
void ArmPauseBeforeRpcForTest(int arrival_number);

// Release a waiter stuck in WaitIfPauseBeforeRpcArmed().
void DisarmPauseBeforeRpcForTest();

// 1 while a call is blocked in WaitIfPauseBeforeRpcArmed().
int PausedBeforeRpcForTest();

// > 0 while the destructor is waiting in WaitUntilIdle().
int IdleWaitBlockedForTest();

// When true, Write() fails starting its reader thread.
void SetFailReaderThreadStartForTest(bool fail);

}  // namespace internal
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_TEST_GRPC_VDISK_CLIENT_TEST_HOOKS_H_
