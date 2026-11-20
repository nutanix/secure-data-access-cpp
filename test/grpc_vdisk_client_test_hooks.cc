/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Test-only controls for the gRPC client hooks. Linked into the smoke tests,
 * not the SDK library.
 */

#include "grpc_vdisk_client_test_hooks.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "internal/grpc_vdisk_client_test_seam.h"

namespace nutanix {
namespace securedata {
namespace sdk {
namespace internal {
namespace {

std::atomic<bool> g_fail_reader_thread_start{false};

// 0 means disarmed. Otherwise the 1-based arrival that should block.
std::mutex g_pause_mu;
std::condition_variable g_pause_cv;
std::atomic<int> g_pause_on_arrival{0};
std::atomic<int> g_pause_arrivals{0};
std::atomic<int> g_paused_before_rpc{0};
std::atomic<int> g_idle_wait_blocked{0};

void WaitIfPauseBeforeRpcArmedForTest() {
  const int gate = g_pause_on_arrival.load(std::memory_order_acquire);
  if (gate <= 0) {
    return;
  }
  const int arrival =
    g_pause_arrivals.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (arrival != gate) {
    return;
  }
  std::unique_lock<std::mutex> lock(g_pause_mu);
  // Disarm may have won the race between the load above and this lock.
  if (g_pause_on_arrival.load(std::memory_order_relaxed) != gate) {
    return;
  }
  g_paused_before_rpc.store(1, std::memory_order_release);
  g_pause_cv.notify_all();
  g_pause_cv.wait(lock, [gate] {
    return g_pause_on_arrival.load(std::memory_order_acquire) != gate;
  });
  g_paused_before_rpc.store(0, std::memory_order_release);
}

bool FailReaderThreadStartHook() { return g_fail_reader_thread_start.load(); }

void NoteIdleWaitBlockedHook() {
  g_idle_wait_blocked.fetch_add(1, std::memory_order_release);
}

void NoteIdleWaitUnblockedHook() {
  g_idle_wait_blocked.fetch_sub(1, std::memory_order_release);
}

const GrpcVDiskClientTestHooks kHooks = {
  &WaitIfPauseBeforeRpcArmedForTest,
  &FailReaderThreadStartHook,
  &NoteIdleWaitBlockedHook,
  &NoteIdleWaitUnblockedHook,
};

struct InstallHooks {
  InstallHooks() { SetGrpcVDiskClientTestHooks(&kHooks); }
};

InstallHooks g_install_hooks;

}  // namespace

void ArmPauseBeforeRpcForTest(int arrival_number) {
  {
    std::lock_guard<std::mutex> lock(g_pause_mu);
    g_pause_arrivals.store(0, std::memory_order_relaxed);
    g_paused_before_rpc.store(0, std::memory_order_relaxed);
    g_pause_on_arrival.store(arrival_number, std::memory_order_release);
  }
  g_pause_cv.notify_all();
}

void DisarmPauseBeforeRpcForTest() {
  {
    std::lock_guard<std::mutex> lock(g_pause_mu);
    g_pause_on_arrival.store(0, std::memory_order_release);
  }
  g_pause_cv.notify_all();
}

int PausedBeforeRpcForTest() {
  return g_paused_before_rpc.load(std::memory_order_acquire);
}

int IdleWaitBlockedForTest() {
  return g_idle_wait_blocked.load(std::memory_order_acquire);
}

void SetFailReaderThreadStartForTest(bool fail) {
  g_fail_reader_thread_start.store(fail);
}

}  // namespace internal
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix
