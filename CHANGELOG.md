# Changelog

All notable changes to Signal are documented in this file.

## 0.1.0

### Added

- Typed enum and integral event IDs with optional trivially copyable payloads.
- Bounded queue, payload, subscription, waiter, and dispatch-match storage.
- Allocation-free raw callback subscriptions with optional context pointers.
- Future-only waiters that broadcast to all matching tasks without consuming subscriber delivery.
- `DropNewest`, `DropOldest`, and `BlockCaller` overflow policies.
- Move-only scoped subscription handles.
- Runtime diagnostics for queue use, callbacks, drops, rejections, waiters, and stack high-water mark.
- Optional PSRAM task stacks with internal-RAM fallback for `SignalStackType::Auto`.
- Deterministic host runtime tests backed by a FreeRTOS compatibility shim.

### Safety

- Serialized `Stopped`, `Initializing`, `Running`, and `Stopping` lifecycle transitions.
- Shutdown-safe tracking for producers blocked outside the internal mutex.
- Cleanup waits for the dispatch task, waiter calls, and blocking post operations to finish.
- Signal callback posts never block the only dispatch task and return `Busy` when no immediate slot exists.
- Timed-out shutdown remains recoverable through a later `end()` call.
- Typed callback reconstruction no longer requires default-constructible payload types.
