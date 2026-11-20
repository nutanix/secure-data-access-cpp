/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * In-process coverage for the callback ReadReactor path: chunk delivery,
 * per-range vs stream-level errors, per-Read() concurrency, and Close().
 */

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "grpc_vdisk_client_test_hooks.h"
#include "internal/grpc_vdisk_client.h"
#include "nutanix/securedata/sdk/api/error_code.h"
#include "nutanix/securedata/sdk/api/read_error.h"
#include "nutanix/securedata/sdk/api/read_handler.h"
#include "nutanix/securedata/sdk/api/read_region.h"
#include "nutanix/securedata/sdk/api/read_regions_request.h"
#include "nutanix/securedata/sdk/api/recovered_chunk.h"
#include "nutanix/securedata/sdk/api/vdisk_client_config.h"
#include "vdisk.grpc.pb.h"
#include "vdisk.pb.h"

// Cert minting uses mkdtemp, rmdir, and a POSIX shell. CMake finds openssl
// and passes its path as SDA_OPENSSL_EXECUTABLE. Without either, the TLS
// cases skip instead of failing the build.
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#define SDA_POSIX_HOST 1
#else
#define SDA_POSIX_HOST 0
#endif

namespace {

using nutanix::securedata::sdk::api::ErrorCode;
using nutanix::securedata::sdk::api::ReadError;
using nutanix::securedata::sdk::api::ReadHandler;
using nutanix::securedata::sdk::api::ReadRegion;
using nutanix::securedata::sdk::api::ReadRegionsRequest;
using nutanix::securedata::sdk::api::RecoveredChunk;
using nutanix::securedata::sdk::api::VDiskClientConfig;
using nutanix::securedata::sdk::internal::ArmPauseBeforeRpcForTest;
using nutanix::securedata::sdk::internal::DisarmPauseBeforeRpcForTest;
using nutanix::securedata::sdk::internal::GrpcVDiskClient;
using nutanix::securedata::sdk::internal::IdleWaitBlockedForTest;
using nutanix::securedata::sdk::internal::PausedBeforeRpcForTest;

// Expands in the test body so GTEST_SKIP() leaves that test, not a helper.
#if !SDA_POSIX_HOST
#define SKIP_WITHOUT_READ_REACTOR_TLS() \
  GTEST_SKIP() << "in-process TLS read tests require a POSIX host"
#elif !defined(SDA_OPENSSL_EXECUTABLE)
#define SKIP_WITHOUT_READ_REACTOR_TLS() \
  GTEST_SKIP()                          \
    << "in-process TLS read tests require openssl; CMake did not find it"
#else
#define SKIP_WITHOUT_READ_REACTOR_TLS() \
  do {                                  \
  } while (0)
#endif

// SDK client always speaks TLS (verify off). Mint a throwaway self-signed
// cert at process start so no key material lives in the source tree.
struct EphemeralServerTls {
  std::string private_key;
  std::string cert_chain;
};

#if SDA_POSIX_HOST && defined(SDA_OPENSSL_EXECUTABLE)
std::string ReadEntireFile(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool GenerateEphemeralServerTls(EphemeralServerTls *out, std::string *error) {
  char tmpl[] = "/tmp/sda_read_reactor_tls_XXXXXX";
  if (mkdtemp(tmpl) == nullptr) {
    *error = "mkdtemp failed for ephemeral TLS material";
    return false;
  }
  const std::string dir(tmpl);
  const std::string key_path = dir + "/key.pem";
  const std::string cert_path = dir + "/cert.pem";

  // Path comes from CMake find_program(openssl), not from the runtime PATH.
  const std::string cmd = std::string("'") + SDA_OPENSSL_EXECUTABLE +
                          "' req -x509 -newkey rsa:2048 -nodes -days 1 "
                          "-subj '/CN=localhost' "
                          "-keyout '" +
                          key_path +
                          "' "
                          "-out '" +
                          cert_path +
                          "' "
                          ">/dev/null 2>&1";
  const int rc = std::system(cmd.c_str());

  out->private_key = ReadEntireFile(key_path);
  out->cert_chain = ReadEntireFile(cert_path);
  std::remove(key_path.c_str());
  std::remove(cert_path.c_str());
  rmdir(dir.c_str());

  if (rc != 0 || out->private_key.empty() || out->cert_chain.empty()) {
    *error = "openssl failed to mint ephemeral TLS material";
    return false;
  }
  return true;
}
#endif

const EphemeralServerTls& TestServerTls() {
  static const EphemeralServerTls tls = [] {
    EphemeralServerTls material;
#if SDA_POSIX_HOST && defined(SDA_OPENSSL_EXECUTABLE)
    std::string error;
    // On failure material stays empty; FakeReadServer::Ok() fails the test.
    (void)GenerateEphemeralServerTls(&material, &error);
#endif
    return material;
  }();
  return tls;
}

class CapturingReadHandler : public ReadHandler {
 public:
  void OnChunk(const RecoveredChunk& chunk) override {
    std::lock_guard<std::mutex> lock(mutex);
    chunks.push_back(chunk);
  }

  void OnError(const ReadError& error) override {
    std::lock_guard<std::mutex> lock(mutex);
    errors.push_back(error);
  }

  std::mutex mutex;
  std::vector<RecoveredChunk> chunks;
  std::vector<ReadError> errors;
};

enum class ReadServerMode {
  kSuccessChunk,
  kPerRangeErrorThenOk,
  kStreamLevelError,
  kHoldUntilCancelled,
  kTrackConcurrency,
  // Each stream stays open until ReleaseHeldReads(), so a test can force
  // two Read() calls to overlap.
  kHoldUntilReleased,
};

class FakeReadService final : public VDiskDataSvc::Service {
 public:
  explicit FakeReadService(ReadServerMode mode) : mode_(mode) {}

  ::grpc::Status VDiskStreamRead(
    ::grpc::ServerContext *context,
    ::grpc::ServerReaderWriter<::VDiskReadRet, ::VDiskReadArg> *stream)
    override {
    ::VDiskReadArg arg;
    if (!stream->Read(&arg)) {
      return ::grpc::Status::OK;
    }

    Enter(arg.offset());
    struct InflightGuard {
      FakeReadService *self;
      int64_t offset;
      ~InflightGuard() { self->Leave(offset); }
    } inflight_guard{this, arg.offset()};

    if (mode_ == ReadServerMode::kHoldUntilReleased) {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [&] { return release_held_ || context->IsCancelled(); });
    }

    if (mode_ == ReadServerMode::kHoldUntilCancelled) {
      while (!context->IsCancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      return ::grpc::Status(::grpc::StatusCode::CANCELLED, "cancelled");
    }

    if (mode_ == ReadServerMode::kTrackConcurrency) {
      // Hold briefly so overlapping StartCall paths can pile up when the
      // client allows more than one in-flight stream.
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (mode_ == ReadServerMode::kStreamLevelError) {
      ::VDiskReadRet ret;
      ret.set_error_code(::kServerBusy);
      ret.set_error_message("stream busy");
      stream->Write(ret);
      return ::grpc::Status::OK;
    }

    if (mode_ == ReadServerMode::kPerRangeErrorThenOk) {
      ::VDiskReadRet err;
      err.set_error_code(::kDiskReadError);
      err.set_error_message("range failed");
      ::DiskDataRange *range = err.add_range_vec();
      range->set_offset(arg.offset());
      range->set_length(1);
      range->set_zero_data(true);
      stream->Write(err);

      ::VDiskReadRet ok;
      ok.set_error_code(::kNoError);
      ::DiskDataRange *ok_range = ok.add_range_vec();
      ok_range->set_offset(arg.offset() + 1);
      ok_range->set_length(arg.length() - 1);
      ok_range->set_zero_data(false);
      ok.set_data(std::string(static_cast<size_t>(arg.length() - 1), 'b'));
      stream->Write(ok);
      return ::grpc::Status::OK;
    }

    // Success (and concurrency-tracking) path: one data chunk for the region.
    ::VDiskReadRet ret;
    ret.set_error_code(::kNoError);
    ::DiskDataRange *range = ret.add_range_vec();
    range->set_offset(arg.offset());
    range->set_length(arg.length());
    range->set_zero_data(false);
    ret.set_data(std::string(static_cast<size_t>(arg.length()), 'a'));
    stream->Write(ret);
    return ::grpc::Status::OK;
  }

  int PeakInflight() const {
    std::lock_guard<std::mutex> lock(mu_);
    return peak_inflight_;
  }

  int Started() const {
    std::lock_guard<std::mutex> lock(mu_);
    return started_;
  }

  // Returns false on timeout so a dead server cannot hang the suite.
  bool WaitUntilStartedAtLeast(
    int n,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout, [&] { return started_ >= n; });
  }

  // Offsets in one group belong to one Read() call.
  void AssignGroup(int64_t offset, int group) {
    std::lock_guard<std::mutex> lock(mu_);
    offset_group_[offset] = group;
  }

  // Unblocks streams parked in kHoldUntilReleased.
  void ReleaseHeldReads() {
    std::lock_guard<std::mutex> lock(mu_);
    release_held_ = true;
    cv_.notify_all();
  }

  // Streams of this Read() that are inside VDiskStreamRead right now.
  int GroupInflight(int group) const {
    std::lock_guard<std::mutex> lock(mu_);
    return GroupInflightLocked(group);
  }

  // Highest number of streams this Read() had open at once.
  int GroupPeak(int group) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = group_peak_.find(group);
    return it == group_peak_.end() ? 0 : it->second;
  }

  // True once each group has at least one stream open. False on timeout.
  bool WaitUntilBothGroupsActive(
    int group_a,
    int group_b,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout, [&] {
      return GroupInflightLocked(group_a) >= 1 &&
             GroupInflightLocked(group_b) >= 1;
    });
  }

 private:
  void Enter(int64_t offset) {
    std::lock_guard<std::mutex> lock(mu_);
    ++inflight_;
    if (inflight_ > peak_inflight_) {
      peak_inflight_ = inflight_;
    }
    ++started_;
    ++inflight_by_offset_[offset];
    const auto group = offset_group_.find(offset);
    if (group != offset_group_.end()) {
      const int sum = GroupInflightLocked(group->second);
      int& peak = group_peak_[group->second];
      if (sum > peak) {
        peak = sum;
      }
    }
    cv_.notify_all();
  }

  void Leave(int64_t offset) {
    std::lock_guard<std::mutex> lock(mu_);
    --inflight_;
    const auto it = inflight_by_offset_.find(offset);
    if (it != inflight_by_offset_.end() && it->second > 0) {
      --it->second;
    }
    cv_.notify_all();
  }

  int GroupInflightLocked(int group) const {
    int sum = 0;
    for (const auto& entry : offset_group_) {
      if (entry.second != group) {
        continue;
      }
      const auto inflight = inflight_by_offset_.find(entry.first);
      if (inflight != inflight_by_offset_.end()) {
        sum += inflight->second;
      }
    }
    return sum;
  }

  ReadServerMode mode_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  int inflight_ = 0;
  int peak_inflight_ = 0;
  int started_ = 0;
  bool release_held_ = false;
  std::map<int64_t, int> inflight_by_offset_;
  std::map<int64_t, int> offset_group_;
  std::map<int, int> group_peak_;
};

class FakeReadServer {
 public:
  explicit FakeReadServer(ReadServerMode mode) : service_(mode) {
    const EphemeralServerTls& tls = TestServerTls();
    if (tls.private_key.empty() || tls.cert_chain.empty()) {
      return;
    }

    ::grpc::SslServerCredentialsOptions::PemKeyCertPair pair;
    pair.private_key = tls.private_key;
    pair.cert_chain = tls.cert_chain;
    ::grpc::SslServerCredentialsOptions ssl_opts(
      GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE);
    ssl_opts.pem_key_cert_pairs.push_back(pair);

    ::grpc::ServerBuilder builder;
    builder.AddListeningPort(
      "127.0.0.1:0",
      ::grpc::SslServerCredentials(ssl_opts),
      &port_);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
  }

  ~FakeReadServer() {
    if (server_) {
      server_->Shutdown();
      server_->Wait();
    }
  }

  bool Ok() const { return server_ != nullptr && port_ > 0; }

  int Port() const { return port_; }

  FakeReadService& Service() { return service_; }

 private:
  FakeReadService service_;
  std::unique_ptr<::grpc::Server> server_;
  int port_ = 0;
};

VDiskClientConfig MakeConfig(int port, int max_concurrent_reads) {
  return VDiskClientConfig::Builder()
    .Host("127.0.0.1")
    .Port(port)
    .AuthToken("TOKEN")
    .MaxConcurrentReads(max_concurrent_reads)
    .Build();
}

TEST(ReadReactorTest, DeliversSuccessChunk) {
  SKIP_WITHOUT_READ_REACTOR_TLS();
  FakeReadServer server(ReadServerMode::kSuccessChunk);
  ASSERT_TRUE(server.Ok()) << "failed to bind fake TLS server on 127.0.0.1";
  GrpcVDiskClient client(MakeConfig(server.Port(), 8));

  CapturingReadHandler handler;
  const std::vector<ReadRegion> regions = {ReadRegion(100, 4)};
  client.Read(ReadRegionsRequest(regions), handler);

  ASSERT_EQ(handler.errors.size(), 0u);
  ASSERT_EQ(handler.chunks.size(), 1u);
  EXPECT_EQ(handler.chunks[0].Offset(), 100);
  EXPECT_EQ(handler.chunks[0].Length(), 4);
  EXPECT_EQ(handler.chunks[0].Data(), "aaaa");
  EXPECT_FALSE(handler.chunks[0].ZeroData());
}

TEST(ReadReactorTest, PerRangeErrorKeepsStreamAndAllowsLaterChunk) {
  SKIP_WITHOUT_READ_REACTOR_TLS();
  FakeReadServer server(ReadServerMode::kPerRangeErrorThenOk);
  ASSERT_TRUE(server.Ok()) << "failed to bind fake TLS server on 127.0.0.1";
  GrpcVDiskClient client(MakeConfig(server.Port(), 8));

  CapturingReadHandler handler;
  const std::vector<ReadRegion> regions = {ReadRegion(50, 5)};
  client.Read(ReadRegionsRequest(regions), handler);

  ASSERT_EQ(handler.errors.size(), 1u);
  EXPECT_EQ(handler.errors[0].Offset(), 50);
  EXPECT_EQ(handler.errors[0].Length(), 1);
  EXPECT_EQ(handler.errors[0].Code(), ErrorCode::kDiskReadError);

  ASSERT_EQ(handler.chunks.size(), 1u);
  EXPECT_EQ(handler.chunks[0].Offset(), 51);
  EXPECT_EQ(handler.chunks[0].Length(), 4);
  EXPECT_EQ(handler.chunks[0].Data(), "bbbb");
}

TEST(ReadReactorTest, StreamLevelErrorReportsOnceInOnDone) {
  SKIP_WITHOUT_READ_REACTOR_TLS();
  FakeReadServer server(ReadServerMode::kStreamLevelError);
  ASSERT_TRUE(server.Ok()) << "failed to bind fake TLS server on 127.0.0.1";
  GrpcVDiskClient client(MakeConfig(server.Port(), 8));

  CapturingReadHandler handler;
  const std::vector<ReadRegion> regions = {ReadRegion(0, 8)};
  client.Read(ReadRegionsRequest(regions), handler);

  ASSERT_EQ(handler.chunks.size(), 0u);
  ASSERT_EQ(handler.errors.size(), 1u);
  EXPECT_EQ(handler.errors[0].Offset(), 0);
  EXPECT_EQ(handler.errors[0].Length(), 8);
  EXPECT_EQ(handler.errors[0].Code(), ErrorCode::kServerBusy);
  EXPECT_NE(handler.errors[0].Message().find("stream busy"), std::string::npos);
}

TEST(ReadReactorTest, MaxConcurrentReadsCapsInFlightStreamsPerCall) {
  SKIP_WITHOUT_READ_REACTOR_TLS();
  FakeReadServer server(ReadServerMode::kTrackConcurrency);
  ASSERT_TRUE(server.Ok()) << "failed to bind fake TLS server on 127.0.0.1";
  GrpcVDiskClient client(MakeConfig(server.Port(), /*max_concurrent_reads=*/1));

  CapturingReadHandler handler;
  const std::vector<ReadRegion> regions = {
    ReadRegion(0, 2),
    ReadRegion(10, 2),
    ReadRegion(20, 2),
  };
  client.Read(ReadRegionsRequest(regions), handler);

  EXPECT_EQ(server.Service().Started(), 3);
  EXPECT_EQ(server.Service().PeakInflight(), 1);
  EXPECT_EQ(handler.errors.size(), 0u);
  EXPECT_EQ(handler.chunks.size(), 3u);
}

TEST(ReadReactorTest, ConcurrentReadCallsEachGetIndependentCap) {
  SKIP_WITHOUT_READ_REACTOR_TLS();
  FakeReadServer server(ReadServerMode::kHoldUntilReleased);
  ASSERT_TRUE(server.Ok()) << "failed to bind fake TLS server on 127.0.0.1";
  GrpcVDiskClient client(MakeConfig(server.Port(), /*max_concurrent_reads=*/1));

  // Group 0 is call A, group 1 is call B. A group's peak is that call's cap.
  server.Service().AssignGroup(0, 0);
  server.Service().AssignGroup(10, 0);
  server.Service().AssignGroup(20, 1);
  server.Service().AssignGroup(30, 1);

  CapturingReadHandler handler_a;
  CapturingReadHandler handler_b;
  const std::vector<ReadRegion> regions_a = {
    ReadRegion(0, 2),
    ReadRegion(10, 2)};
  const std::vector<ReadRegion> regions_b = {
    ReadRegion(20, 2),
    ReadRegion(30, 2)};

  {
    std::thread t_a(
      [&] { client.Read(ReadRegionsRequest(regions_a), handler_a); });
    std::thread t_b(
      [&] { client.Read(ReadRegionsRequest(regions_b), handler_b); });
    // Release and join even when an assertion below fails.
    struct FinishReads {
      FakeReadService *service;
      std::thread *a;
      std::thread *b;
      ~FinishReads() {
        service->ReleaseHeldReads();
        if (a->joinable()) {
          a->join();
        }
        if (b->joinable()) {
          b->join();
        }
      }
    } finish{&server.Service(), &t_a, &t_b};

    ASSERT_TRUE(server.Service().WaitUntilBothGroupsActive(0, 1))
      << "the two Read() calls did not overlap";
    // Each call is still on its first region, so its own cap is 1, while
    // both calls are open together.
    EXPECT_EQ(server.Service().GroupInflight(0), 1);
    EXPECT_EQ(server.Service().GroupInflight(1), 1);
    EXPECT_GE(server.Service().PeakInflight(), 2);
  }

  EXPECT_EQ(server.Service().GroupPeak(0), 1);
  EXPECT_EQ(server.Service().GroupPeak(1), 1);
  EXPECT_EQ(server.Service().PeakInflight(), 2);
  EXPECT_EQ(server.Service().Started(), 4);
  EXPECT_EQ(handler_a.errors.size(), 0u);
  EXPECT_EQ(handler_b.errors.size(), 0u);
  EXPECT_EQ(handler_a.chunks.size(), 2u);
  EXPECT_EQ(handler_b.chunks.size(), 2u);
}

TEST(ReadReactorTest, CloseCancelsInFlightRead) {
  SKIP_WITHOUT_READ_REACTOR_TLS();
  FakeReadServer server(ReadServerMode::kHoldUntilCancelled);
  ASSERT_TRUE(server.Ok()) << "failed to bind fake TLS server on 127.0.0.1";
  GrpcVDiskClient client(MakeConfig(server.Port(), 8));

  CapturingReadHandler handler;
  const std::vector<ReadRegion> regions = {ReadRegion(0, 4)};

  std::thread reader(
    [&] { client.Read(ReadRegionsRequest(regions), handler); });
  ASSERT_TRUE(server.Service().WaitUntilStartedAtLeast(1))
    << "server never saw the read stream";
  client.Close();
  reader.join();

  ASSERT_EQ(handler.errors.size(), 1u);
  EXPECT_EQ(handler.errors[0].Code(), ErrorCode::kCancelled);
  EXPECT_EQ(handler.chunks.size(), 0u);
}

TEST(ReadReactorTest, ClosedClientRejectsReadWithoutStartingStreams) {
  SKIP_WITHOUT_READ_REACTOR_TLS();
  FakeReadServer server(ReadServerMode::kSuccessChunk);
  ASSERT_TRUE(server.Ok()) << "failed to bind fake TLS server on 127.0.0.1";
  GrpcVDiskClient client(MakeConfig(server.Port(), 8));
  client.Close();

  CapturingReadHandler handler;
  const std::vector<ReadRegion> regions = {ReadRegion(0, 4), ReadRegion(10, 4)};
  client.Read(ReadRegionsRequest(regions), handler);

  EXPECT_EQ(server.Service().Started(), 0);
  ASSERT_EQ(handler.errors.size(), 2u);
  EXPECT_EQ(handler.errors[0].Code(), ErrorCode::kCancelled);
  EXPECT_EQ(handler.errors[1].Code(), ErrorCode::kCancelled);
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

// Disarms the pre-RPC pause and joins worker threads if the test bails out
// before doing that itself.
struct PauseCleanup {
  std::thread *first = nullptr;
  std::thread *second = nullptr;
  bool joined = false;

  ~PauseCleanup() {
    DisarmPauseBeforeRpcForTest();
    if (joined) {
      return;
    }
    if (first != nullptr && first->joinable()) {
      first->join();
    }
    if (second != nullptr && second->joinable()) {
      second->join();
    }
  }

  void Join() {
    DisarmPauseBeforeRpcForTest();
    if (first != nullptr && first->joinable()) {
      first->join();
    }
    if (second != nullptr && second->joinable()) {
      second->join();
    }
    joined = true;
  }
};

// Destroy the client while Read() is inside the call but has not registered
// a context. The destructor must block until that caller returns.
TEST(ReadReactorTest, DestructorWaitsBeforeFirstReadContext) {
  ArmPauseBeforeRpcForTest(1);

  const VDiskClientConfig config = VDiskClientConfig::Builder()
                                     .Host("127.0.0.1")
                                     .Port(9)
                                     .AuthToken("TOKEN")
                                     .MaxConcurrentReads(1)
                                     .Build();
  auto client = std::make_unique<GrpcVDiskClient>(config);
  CapturingReadHandler handler;
  const std::vector<ReadRegion> regions = {ReadRegion(0, 4), ReadRegion(10, 4)};

  std::atomic<bool> destroyed{false};
  std::thread reader(
    [&] { client->Read(ReadRegionsRequest(regions), handler); });
  std::thread destroyer;
  PauseCleanup cleanup;
  cleanup.first = &reader;
  cleanup.second = &destroyer;

  ASSERT_TRUE(WaitUntil(
    [] { return PausedBeforeRpcForTest() > 0; },
    std::chrono::seconds(5)))
    << "Read() did not reach the pre-context pause";

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

  ASSERT_EQ(handler.errors.size(), 2u);
  EXPECT_EQ(handler.errors[0].Code(), ErrorCode::kCancelled);
  EXPECT_EQ(handler.errors[1].Code(), ErrorCode::kCancelled);
  EXPECT_EQ(handler.chunks.size(), 0u);
}

// MaxConcurrentReads(1): after region 1's context is unregistered and before
// region 2's is registered, active_contexts_ is empty while Read() is still
// in the call. Destruction must wait for that caller.
TEST(ReadReactorTest, DestructorWaitsBetweenReadRegions) {
  SKIP_WITHOUT_READ_REACTOR_TLS();
  FakeReadServer server(ReadServerMode::kSuccessChunk);
  ASSERT_TRUE(server.Ok()) << "failed to bind fake TLS server on 127.0.0.1";
  ArmPauseBeforeRpcForTest(2);

  auto client = std::make_unique<GrpcVDiskClient>(MakeConfig(server.Port(), 1));
  CapturingReadHandler handler;
  const std::vector<ReadRegion> regions = {ReadRegion(0, 4), ReadRegion(10, 4)};

  std::atomic<bool> destroyed{false};
  std::thread reader(
    [&] { client->Read(ReadRegionsRequest(regions), handler); });
  std::thread destroyer;
  PauseCleanup cleanup;
  cleanup.first = &reader;
  cleanup.second = &destroyer;

  ASSERT_TRUE(WaitUntil(
    [] { return PausedBeforeRpcForTest() > 0; },
    std::chrono::seconds(5)))
    << "Read() did not pause between regions";

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

  ASSERT_EQ(handler.chunks.size(), 1u);
  EXPECT_EQ(handler.chunks[0].Offset(), 0);
  EXPECT_EQ(handler.chunks[0].Length(), 4);
  ASSERT_EQ(handler.errors.size(), 1u);
  EXPECT_EQ(handler.errors[0].Offset(), 10);
  EXPECT_EQ(handler.errors[0].Code(), ErrorCode::kCancelled);
}

}  // namespace
