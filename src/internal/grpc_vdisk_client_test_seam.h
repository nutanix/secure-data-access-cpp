/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Optional hooks the gRPC client calls. Production leaves them unset, so each
 * call returns immediately. Tests install the real functions.
 */

#ifndef NUTANIX_SECUREDATA_SDK_INTERNAL_GRPC_VDISK_CLIENT_TEST_SEAM_H_
#define NUTANIX_SECUREDATA_SDK_INTERNAL_GRPC_VDISK_CLIENT_TEST_SEAM_H_

namespace nutanix {
namespace securedata {
namespace sdk {
namespace internal {

struct GrpcVDiskClientTestHooks {
  void (*wait_if_pause_before_rpc)() = nullptr;
  bool (*fail_reader_thread_start)() = nullptr;
  void (*note_idle_wait_blocked)() = nullptr;
  void (*note_idle_wait_unblocked)() = nullptr;
};

// Replaces any previously installed hooks. Null clears them.
void SetGrpcVDiskClientTestHooks(const GrpcVDiskClientTestHooks *hooks);

// No-op unless a test installed a hook.
void WaitIfPauseBeforeRpcArmed();
bool FailReaderThreadStartForTest();
void NoteIdleWaitBlockedForTest();
void NoteIdleWaitUnblockedForTest();

}  // namespace internal
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_INTERNAL_GRPC_VDISK_CLIENT_TEST_SEAM_H_
