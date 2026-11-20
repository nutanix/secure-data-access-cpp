/*
 * Copyright (c) 2026 Nutanix Inc. All rights reserved.
 *
 * Author: asis.bagga@nutanix.com
 *
 * Callback sink for streamed recovered chunks and per-range read errors.
 */

#ifndef NUTANIX_SECUREDATA_SDK_API_READ_HANDLER_H_
#define NUTANIX_SECUREDATA_SDK_API_READ_HANDLER_H_

#include "nutanix/securedata/sdk/api/read_error.h"
#include "nutanix/securedata/sdk/api/recovered_chunk.h"

namespace nutanix {
namespace securedata {
namespace sdk {
namespace api {

// Sink for the results of a multi-region read. A 'ReadHandler' exposes exactly
// two callbacks and no more: 'OnChunk' for recovered data and 'OnError' for
// failures. This is the entire contract a caller implements to consume a read;
// there are no other hooks, no lifecycle methods, and no return values.
//
// How a read is delivered:
//   Reads stream. Instead of returning one large buffer, the SDK invokes
//   'OnChunk' as each chunk arrives and 'OnError' as each range fails. The
//   read call returns only after every region has completed, so once it
//   returns no further callbacks will fire for that request.
//
// Threading:
//   Regions are fetched concurrently on independent streams; the SDK does NOT
//   serialize callbacks. 'OnChunk' and 'OnError' may be invoked concurrently
//   on multiple threads, so implementations must be thread-safe. Keep each
//   callback short and non-blocking; it runs on an SDK transport thread.
//
// Client lifetime (API contract — misuse otherwise):
//   Do NOT call 'VDiskClient::Close()' or destroy the client from 'OnChunk' or
//   'OnError'. That is unsupported API misuse. Record failures in your own
//   state; let the client close itself when the owning scope ends after
//   'Read()' / 'ReadRegions()' returns. To abort from another thread, call
//   'Close()' on that other thread — never from inside a handler callback.
//
// What each callback owns:
//   Every 'RecoveredChunk' and 'ReadError' is self-locating via its absolute
//   vdisk offset, so a handler can persist data or schedule retries directly
//   without buffering or reordering. 'RecoveredChunk::Data()' is a non-owning
//   view. If a caller retains the 'RecoveredChunk' object, the view remains
//   valid for that object's lifetime.
//
// If a callback throws:
//   Exceptions thrown from 'OnChunk' or 'OnError' are the SDK's to contain:
//   the SDK logs one mutex-protected diagnostic line to stderr and suppresses
//   the exception. Logging/formatting failures are also suppressed and cannot
//   escape the SDK. Callback exceptions do NOT propagate out of the read call,
//   do not stop other chunks or regions, and never leak internal stream
//   bookkeeping. Signal problems through your own state instead of throwing.
class ReadHandler {
 public:
  virtual ~ReadHandler() = default;

  // Invoked once per recovered chunk. The chunk is self-locating via its
  // absolute 'RecoveredChunk::Offset()'; for a zero-data chunk the data is
  // empty and 'RecoveredChunk::Length()' tells how many zeros to materialize.
  // A retained 'RecoveredChunk' keeps its own payload lifetime.
  virtual void OnChunk(const RecoveredChunk& chunk) = 0;

  // Invoked once per failed range. The failing '[offset, length)' is reported
  // so the caller can retry exactly that range, optionally keyed off
  // 'ReadError::Code()'.
  virtual void OnError(const ReadError& error) = 0;
};

}  // namespace api
}  // namespace sdk
}  // namespace securedata
}  // namespace nutanix

#endif  // NUTANIX_SECUREDATA_SDK_API_READ_HANDLER_H_
