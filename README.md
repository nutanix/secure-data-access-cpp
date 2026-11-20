# Secure Data C++ SDK - Client Usage Guide

This SDK provides a simple client interface to read and write vdisk regions for
backup and restore workflows. It is built for streaming large transfers with
typed per-region success/error reporting and bounded client memory usage.

## What you use from the SDK

As a consumer, you mainly use one client and a set of public API types for regions, handlers, and errors.

```cpp
#include "nutanix/securedata/sdk/secure_data_access.h"
```

Core type:

- `nutanix::securedata::sdk::SecureDataAccess::Client` — a connection to one
  vdisk. It owns client resources and closes active operations on destruction
  (RAII).

The two operations are region-based and stream their results through a handler:

- `ReadRegions(std::vector<ReadRegion>&& regions, ReadHandler& handler)`
- `WriteRegions(std::vector<WriteRegion>&& regions, const WriteDataProvider& provider, WriteHandler& handler)`

Everything else you touch lives in `nutanix::securedata::sdk::api`:

- `ReadRegion`, `ReadHandler`, `RecoveredChunk`, `ReadError`
- `WriteRegion`, `WriteHandler`, `WriteDataProvider`, `WriteAck`, `WriteError`
- `ErrorCode`

## Building

Protocol-buffer and RPC runtime dependencies are provided by the environment
and discovered via CMake's `find_package`. Point CMake at them (for example a
Homebrew or system prefix):

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix)" -DSDA_BUILD_TESTS=ON
cmake --build build --target sda_smoke_tests -j
ctest --test-dir build --output-on-failure
```

Note: committed files under `generated/` are currently produced by protoc
35.1-compatible tooling, so your protobuf C++ headers/runtime must be
compatible with that generation version.

Link your application against the `secure_data_access` target.

## 1) Create the client

```cpp
using nutanix::securedata::sdk::SecureDataAccess;

std::string host = "10.96.84.198"; // CVM IP or DNS reachable from your client
int port = 14231;
std::string token = "TOKEN";        // bearer token; also identifies the disk

SecureDataAccess::Client client(host, port, token);
// use client here; it closes on scope exit
```

### Cancelling Transfers and Client Shutdown

To cancel an ongoing `ReadRegions()` or `WriteRegions()` operation (for
example, if a user cancels a backup or a backup job times out on the host
side), call `Close()` on your `SecureDataAccess::Client` instance from
**another thread** (not from inside a handler callback).

#### Behavior:
* **Thread-Safe Abort:** Calling `Close()` from another thread immediately
  cancels all active operations in flight.
* **Clean Error Handling:** Active worker threads will unblock cleanly and pass an `ErrorCode::kCancelled` error to the provided `ReadHandler` or `WriteHandler`.
* **Idempotent:** `Close()` can be safely called multiple times. Subsequent
  calls to `ReadRegions()` or `WriteRegions()` on a closed client immediately
  fail with `ErrorCode::kCancelled`.
* **Automatic Destruction:** The client's destructor (`SecureDataAccess::Client::~Client()`) calls `Close()` automatically to prevent dangling network streams upon deletion. Prefer letting the client close itself when it leaves scope after `ReadRegions()` / `WriteRegions()` returns.

#### Example:

```cpp
#include <thread>

std::thread worker([&] {
  client.ReadRegions(std::move(regions), handler);
});

if (user_requested_cancel) {
  client.Close(); // Cancels in-flight calls and unblocks worker threads
}
worker.join();
```

#### Keepalive notes:

- `keep_alive_time_seconds` is the keepalive ping interval and
  `keep_alive_timeout_seconds` is the ping-ack wait timeout; these are not
  whole-operation deadlines.
- If you pass only one keepalive value, the other uses the SDK default.
- If both are omitted, both SDK defaults are used (disabled by default).
- If either resolved keepalive value is `0`, keepalive is disabled.
- When keepalive is enabled (both resolved values > 0), timeout must be
  strictly smaller than interval (`keepAliveTimeout < keepAliveTime`).
- Keepalive detects connection liveness during an active call; it does not
  enforce application-level progress. An operation can remain open while making
  no useful progress if the connection is still alive.
- For finer control, build a `VDiskClientConfig` (via its `Builder`) with
  `KeepAliveTime`, `KeepAliveTimeout`, and `OverallDeadline` (an optional hard
  cap on the whole operation, default **unbounded**), then pass it to
  `VDiskClients::Create`. The SDK applies `OverallDeadline` to each read/write
  call; when it expires, the operation is cancelled and reported as
  `ErrorCode::kDeadlineExceeded`.
- Callers can also abort in-flight operations explicitly at any time by calling
  `Close()`. Active calls are cancelled and reported as
  `ErrorCode::kCancelled`.

#### Connection notes:

- The client always uses TLS, with server-certificate verification skipped.
  Authorization comes from the bearer `token`, not from server certificate
  identity, so there is no CA certificate, server name, or "skip verify" flag
  to pass.
- The `token` authenticates the caller and also carries the disk access scope.
  The server derives which disk can be read/written from this token context, so
  the SDK holds no disk details of its own. To access a different disk, use a
  token scoped for that disk.

Optional read-concurrency cap:

```cpp
int max_concurrent_reads = 16; // defaults to 8
SecureDataAccess::Client client(
    host,
    port,
    token,
    std::nullopt,
    max_concurrent_reads);
```

Optional keepalive enablement:

```cpp
std::optional<int64_t> keep_alive_time_seconds = 120;
std::optional<int64_t> keep_alive_timeout_seconds = 15;
SecureDataAccess::Client client(
    host,
    port,
    token,
    keep_alive_time_seconds,
    max_concurrent_reads,
    keep_alive_timeout_seconds);
```

`max_concurrent_reads` is a per-`ReadRegions()` cap on how many read streams
the SDK keeps in flight within one call. Extra regions are queued and started
as earlier reads finish, so the cap bounds per-call concurrency without
limiting how many regions a single call may pass.

If the same client issues multiple `ReadRegions()` calls concurrently, each
call applies this cap independently (for example two concurrent calls can each
use up to `max_concurrent_reads` streams).

The server's active-read-stream limit is **process-wide and shared** across
every client (default 64 on the server), not a per-client budget, so the SDK
default is a conservative `8` to avoid one client saturating the server and
starving other jobs. Raise it only when the client has the server largely to
itself. A stream the server refuses past its limit is reported as
`ErrorCode::kServerBusy` — a back-off signal to retry the whole read later, not
a per-range data failure.

## 2) Read

`ReadRegions` fetches one or more, possibly non-contiguous, regions in a single
call. A plain contiguous read is just a one-element list; an incremental-backup
read passes the changed extents as multiple regions. Each region is fetched on
its own stream (the server allows only one read per stream); the SDK keeps a
bounded number of streams in flight and queues the rest, so the region list can
be arbitrarily long.

Results stream through a `ReadHandler`: `OnChunk` receives each recovered chunk
as it arrives, and `OnError` receives each failed range. Callbacks are **not**
serialized and may run concurrently on independent threads, so the handler must
be thread-safe. Because every chunk and error carries its absolute vdisk offset,
callers can persist data directly and retry exactly the ranges that failed —
there is no aggregate return value. The call returns only after every region has
completed.

`RecoveredChunk::Data()` is a non-owning view. A retained `RecoveredChunk`
keeps shared ownership of its payload so the view remains valid for the chunk's
lifetime. This enables queueing chunks for asynchronous writes/checksums
without an immediate copy.

```cpp
using namespace nutanix::securedata::sdk::api;

class MyReadHandler : public ReadHandler {
 public:
  void OnChunk(const RecoveredChunk& chunk) override {
    // chunk.Offset() is the absolute vdisk offset — write it there directly.
    // When chunk.ZeroData() is true, chunk.Data() is empty and chunk.Length()
    // tells how many zeros to materialize.
  }

  void OnError(const ReadError& error) override {
    // Retry exactly [error.Offset(), error.Length()); error.Code() is typed.
  }
};

std::vector<ReadRegion> regions = {
    ReadRegion(0, 1 << 20),           // offset, length
    ReadRegion(1LL << 30, 1 << 20),
};

MyReadHandler handler;
client.ReadRegions(std::move(regions), handler);
```

## 3) Write

`WriteRegions` writes one or more, possibly non-contiguous, regions in a single
call. A plain contiguous write is a one-element list; an incremental-backup
restore passes the changed extents (real data or zero-fill holes) as multiple
regions. Each region maps to one request, pipelined on a single stream and
acknowledged independently. The SDK keeps a bounded number of requests in flight
and queues the rest, so the region list can be arbitrarily long.

A region carries no bytes — it only says where and how much to write. Build one
with `WriteRegion::OfData(offset, length)` for real data or
`WriteRegion::OfZeros(offset, length)` for a zero-fill hole. The bytes for data
regions are pulled from the `WriteDataProvider` on the fly at send time (one
range at a time), so the caller never holds the whole payload in memory.

Results stream through a `WriteHandler`: `OnSuccess` receives each acknowledged
region and `OnError` receives each failed region, both keyed by absolute offset
and length so a caller can retry exactly what failed. Callbacks may run
concurrently, so the handler must be thread-safe. The call returns once every
region has reported.

```cpp
using namespace nutanix::securedata::sdk::api;

class MyWriteHandler : public WriteHandler {
 public:
  void OnSuccess(const WriteAck& ack) override {
    std::cout << "wrote offset=" << ack.Offset()
              << " length=" << ack.Length() << std::endl;
  }
  void OnError(const WriteError& error) override {
    std::cerr << "FAILED offset=" << error.Offset()
              << " length=" << error.Length()
              << " code=" << ErrorCodeName(error.Code())
              << " message=" << error.Message() << std::endl;
  }
};

std::string payload = "hello secure data";
std::vector<WriteRegion> regions = {
    WriteRegion::OfData(1LL << 30, static_cast<int32_t>(payload.size())),
};

MyWriteHandler handler;
// Data provider: the SDK asks for a range and you return exactly that many
// bytes, produced on the fly. Return exclusive std::string ownership for the
// fast path: SDK transfers it into protobuf (set_allocated_data), avoiding an
// extra provider-to-protobuf copy.
WriteDataProvider provider =
    [&](int64_t offset, int32_t length) {
      auto owned = std::unique_ptr<std::string>(new std::string(payload));
      return OwnedWriteBuffer(std::move(owned));
    };

client.WriteRegions(std::move(regions), provider, handler);
```

Write behavior notes:

- `WriteRegion` length is a 32-bit value; a wire range's length is int32. To
  write more than `INT32_MAX` bytes, split the work across multiple regions.
- Each region maps to a single request, so its data must fit in one request.
  Keep each region at or below the per-request cap (`MaxWriteBytesPerRequest`,
  default 30 MiB). A region whose data exceeds the cap is not sent — it is
  reported via `WriteHandler::OnError` with `ErrorCode::kInvalidArgument`.
- Zero-fill regions send no bytes and never consult the provider. The SDK does
  not scan data regions for zeros, so a caller that knows a region is empty —
  for example from changed-region tracking — should declare it a hole with
  `WriteRegion::OfZeros`.
- A provider that returns the wrong number of bytes, or throws, fails only that
  one region; the remaining regions still send.
- `OwnedWriteBuffer` has two modes:
  - `OwnedWriteBuffer(std::unique_ptr<std::string>)`: transfer path (no
    provider-to-protobuf copy).
  - `OwnedWriteBuffer(shared_owner, std::string_view)`: fallback path for
    shared/non-owning storage; SDK must copy into protobuf storage.

## 4) Handling errors and retries

Both `ReadError` and `WriteError` expose a typed `ErrorCode` (via `Code()`) so
you can drive a retry policy without parsing strings:

- `kInternalError`
- `kInvalidArgument`
- `kDiskNotFound`
- `kDiskReadError`
- `kDiskWriteError`
- `kPermissionDenied`
- `kConcurrentRequestNotSupported`
- `kServerBusy`
- `kRetry`
- `kDeadlineExceeded`
- `kCancelled`
- `kUnknown` (a code newer than this SDK build; upgrade the SDK to interpret it)

Whether a given code is worth retrying is left to your policy. Because each
error carries the exact `[offset, length)` that failed, retrying means
resubmitting just that range or region.

Handler threading and exceptions:

- Handlers may be invoked concurrently on SDK transport threads. Guard shared
  mutable state with your own synchronization and keep callbacks short and
  non-blocking.
- Exceptions thrown from a handler callback are suppressed by the SDK; they do
  not propagate out of the call, cancel the operation, or stop other regions.
  Signal problems through your own state rather than by throwing.
- Do not call `Close()` or destroy the client from `OnError` / `OnChunk` /
  `OnSuccess`. That is API misuse. Let the client close itself after
  `ReadRegions()` / `WriteRegions()` returns (or call `Close()` only from
  another thread to abort).

## 5) Tuning

Connection-level tuning is set when constructing the client. The facade exposes
`max_concurrent_reads` directly (see section 1). The remaining knobs use
server-derived defaults chosen to match the server's caps, and are surfaced on
`VDiskClientConfig` for advanced use:

- `MaxConcurrentReads` (default 8) — per-`ReadRegions()` read streams kept in
  flight at once (conservative: the server's cap is process-wide and shared).
- `MaxWriteBytesPerRequest` (default 30 MiB) — largest payload allowed in one
  write request.
- `MaxInFlightWriteRequests` (default 20) — write requests kept in flight
  (sent, unacked).
- `MaxInFlightWriteBytes` (default 30 MiB) — aggregate unacked write payload;
  bounds peak client memory. Must be `>= MaxWriteBytesPerRequest`.
- `KeepAliveTime` (default 30s) — HTTP/2 keepalive ping interval while a call
  is active. Zero disables keepalive.
- `KeepAliveTimeout` (default 20s) — how long to wait for a ping ack before
  treating the connection as dead. Must be `< KeepAliveTime` when enabled.

The write byte budget (`MaxInFlightWriteBytes`) bounds peak memory independently
of the request count: the sender blocks until the budget has room before pulling
the next region's bytes, so large regions throttle by bytes rather than only by
count. With the defaults, one 30 MiB region fills the byte budget, and further
regions wait until that write is acknowledged.

## 6) Execution model note

Reads and writes are executed with helper threads behind the client interface.
Reads run a bounded worker pool across regions; writes use one helper thread to
drain acknowledgements while the calling thread pipelines sends. Behavior
(bounded concurrency, unordered chunk delivery, exactly-once per-region
reporting, typed error mapping).

## 7) Bundled examples

Runnable examples live under `examples/`:

- `backup_read_example` — incremental-backup read: streams back only the changed
  (CBT `REGULAR`) regions; `ZEROED` regions are recorded as holes and never
  read.
- `restore_write_example` — restore write: replays several non-contiguous
  regions, including zero-fill holes, from a single `WriteDataProvider` in one
  call.

They read connection settings from environment variables (with defaults):
`VDISK_HOST` (`127.0.0.1`), `VDISK_PORT` (`14231`), `VDISK_TOKEN` (`TOKEN`),
For `backup_read_example`, also set
`VDISK_OFFSET` (`0`) and `VDISK_LENGTH` (`1048576`).

## 8) Running examples from CLI

Backup read (incremental):

```bash
VDISK_HOST=10.96.84.198 VDISK_PORT=14231 VDISK_TOKEN=TOKEN \
  VDISK_OFFSET=0 VDISK_LENGTH=1048576 \
  ./build/backup_read_example
```

Restore write:

```bash
VDISK_HOST=10.96.84.198 VDISK_PORT=14231 VDISK_TOKEN=TOKEN \
  ./build/restore_write_example
```

# Build In A Fresh Environment

If this is the first time building on a machine, use this checklist.

## 1) Prerequisites

- C++17 compiler (`clang++` or `g++`)
- CMake `>= 3.16`
- `protobuf` development package (headers + library)
- `grpc` C++ development package
- `gtest` (needed for `ctest` / smoke tests)

Quick check:

```bash
cmake --version
clang++ --version   # or: g++ --version
```

## 2) Install Dependencies

Use one package manager path consistently on a machine.

macOS (Homebrew):

```bash
brew install cmake protobuf grpc googletest re2 abseil
```

Ubuntu/Debian (example):

```bash
sudo apt-get update
sudo apt-get install -y cmake g++ pkg-config protobuf-compiler libprotobuf-dev \
  libgrpc++-dev libgtest-dev libre2-dev
```

## 3) Configure From Repo Root

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix)"
```

If you are not using Homebrew, set `CMAKE_PREFIX_PATH` to the install prefix
that contains your dependency CMake config files.

## 4) Build And Test

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## 5) Build Examples (optional quick validation)

Examples are enabled by default for top-level builds. Run:

```bash
./build/backup_read_example
./build/restore_write_example
```

### Common Build Issues

- **`Could NOT find Protobuf`**  
  Ensure protobuf dev files are installed and included in `CMAKE_PREFIX_PATH`.
- **`Could NOT find gRPC`**  
  Install gRPC C++ package and make sure CMake can see its config path.
- **GTest not found**  
  Install `googletest`/`gtest` package, or configure with
  `-DSDA_BUILD_TESTS=OFF` if you only need the library build.
- **Stale build cache after dependency changes**  
  Remove `build/` and re-run configure:
  `rm -rf build && cmake -S . -B build ...`