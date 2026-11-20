/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Callback sink for per-region write acknowledgements and errors.
 */

#ifndef NUTANIX_SECUREDATA_SDK_API_WRITE_HANDLER_H_
#define NUTANIX_SECUREDATA_SDK_API_WRITE_HANDLER_H_

#include "nutanix/securedata/sdk/api/write_ack.h"
#include "nutanix/securedata/sdk/api/write_error.h"

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

// Sink for the results of a multi-region write. A 'WriteHandler' exposes
// exactly two callbacks and no more: 'OnSuccess' for regions the server
// acknowledged and 'OnError' for regions that failed. This is the entire
// contract a caller implements to observe a write; there are no other hooks
// and no aggregate return value.
//
// How a write is delivered:
//   Writes pipeline: regions are streamed to the server and acknowledged
//   independently, so a single write may invoke 'OnSuccess' and 'OnError' any
//   number of times in any interleaving. Each region reports exactly once --
//   as a success or an error, never both. The write call returns only after
//   every region has reported, so once it returns no further callbacks fire.
//
// Threading:
//   Callbacks are not serialized by the SDK and may be invoked concurrently on
//   multiple threads (server acknowledgements arrive on a transport thread,
//   while a region rejected before it is sent is reported on the calling
//   thread). Implementations must be thread-safe; guard shared mutable state
//   with your own synchronization and keep callbacks short.
//
// Client lifetime (API contract — misuse otherwise):
//   Do NOT call 'VDiskClient::Close()' or destroy the client from 'OnSuccess' or
//   'OnError'. That is unsupported API misuse. Record failures in your own
//   state; let the client close itself when the owning scope ends after
//   'Write()' / 'WriteRegions()' returns. To abort from another thread, call
//   'Close()' on that other thread — never from inside a handler callback.
//
// What each callback owns:
//   Both 'WriteAck' and 'WriteError' identify their region by absolute offset
//   and length (If its not a tansport layer error) -- the coordinates the caller
//   submitted -- so a handler can track progress or retry failed regions directly.
//
// If a callback throws:
//   Exceptions thrown from 'OnSuccess' or 'OnError' are the SDK's to contain:
//   the SDK logs one mutex-protected diagnostic line to stderr and suppresses
//   the exception. Logging/formatting failures are also suppressed and cannot
//   escape the SDK. Callback exceptions do NOT propagate out of the write
//   call, do not cancel the shared write stream, and do not stop the remaining
//   regions from being reported. Signal problems through your own state instead
//   of throwing.
class WriteHandler {
 public:
  virtual ~WriteHandler() = default;

  // Invoked once per region the server acknowledged as written.
  virtual void OnSuccess(const WriteAck& ack) = 0;

  // Represents a write failure for a specific region, delivered to
  // 'WriteHandler::OnError' when a region is rejected, cannot be sent,
  // or is terminated mid-stream.
  //
  // SELF-LOCATING COORDINATES:
  // 'Offset()' and 'Length()' describe the exact region that failed -- matching
  // the coordinates submitted by the caller -- allowing precise retry targeting.
  // 'Code()' provides a typed reason (see 'ErrorCode') to drive retry workflows.
  //
  // ERROR SEMANTICS & RETRY CONTRACT:
  //
  // 1. DISCRETE REGION ERRORS (e.g., kInvalidArgument, kInternalError during stream):
  //    - Occurs when a specific region is rejected by client validation, data provider
  //      failures, or explicit server-side checks for that sequence number.
  //    - The stream remains healthy; surrounding regions continue processing normally.
  //    - Outcome: DEFINITELY NOT COMMITTED. The region was safely aborted without disk impact.
  //
  // 2. TERMINAL / STREAM-FATAL ERRORS (e.g., kCancelled, kUnavailable during teardown):
  //    - Occurs when the underlying transport breaks or 'TryCancel()' tears down
  //      the stream early (e.g., during client shutdown or stream failure).
  //    - Delivered via the drain phase for all unacknowledged (in-flight) and unsent regions.
  //    - Outcome: AMBIGUOUS for in-flight regions. The server MAY have committed the write
  //      to disk immediately before transport teardown or TCP RST arrival.
  //    - Caller Requirement: Recovery/retry loops MUST be IDEMPOTENT (e.g., blindly
  //      overwriting or re-verifying the region offset).
  virtual void OnError(const WriteError& error) = 0;
};

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_API_WRITE_HANDLER_H_
