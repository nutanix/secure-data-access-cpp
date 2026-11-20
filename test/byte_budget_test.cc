/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Smoke tests for the write byte budget and counting semaphore gates.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "internal/grpc_vdisk_client_support.h"

namespace {

using nutanix::securedata::sdk::internal::ByteBudget;
using nutanix::securedata::sdk::internal::CountingSemaphore;

TEST(ByteBudgetTest, AcquireReleaseCycles) {
  ByteBudget budget(1024);
  budget.Acquire(512);
  budget.Acquire(512);
  // Budget is full; release makes room again.
  budget.Release(512);
  budget.Acquire(512);
  budget.Release(1024);
  SUCCEED();
}

TEST(ByteBudgetTest, BlocksUntilRoomFrees) {
  ByteBudget budget(1024);
  budget.Acquire(1024);

  std::atomic<bool> acquired{false};
  std::thread waiter([&] {
    budget.Acquire(512);
    acquired.store(true);
    budget.Release(512);
  });

  // The waiter must be blocked while the budget is saturated.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(acquired.load());

  budget.Release(1024);
  waiter.join();
  EXPECT_TRUE(acquired.load());
}

TEST(ByteBudgetTest, AbortWakesWaiters) {
  ByteBudget budget(1024);
  budget.Acquire(1024);

  std::atomic<bool> returned{false};
  std::thread waiter([&] {
    // Would otherwise block forever; abort must let it proceed.
    budget.Acquire(512);
    returned.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(returned.load());

  budget.Abort();
  waiter.join();
  EXPECT_TRUE(returned.load());
}

TEST(CountingSemaphoreTest, BoundsConcurrency) {
  CountingSemaphore sem(2);
  sem.Acquire();
  sem.Acquire();

  std::atomic<bool> acquired_third{false};
  std::thread waiter([&] {
    sem.Acquire();
    acquired_third.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(acquired_third.load());

  sem.Release();
  waiter.join();
  EXPECT_TRUE(acquired_third.load());
}

}  // namespace
