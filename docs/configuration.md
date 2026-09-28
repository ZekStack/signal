# Configuration

`SignalConfig` controls task behavior, bounded storage, and the shared Strata memory policy.

```cpp
SignalConfig config;
config.memory.allocation = Strata::Placement::Default;
config.memory.taskStack = Strata::Placement::PreferExternal;
config.stackSizeBytes = 4096;
config.priority = 1;
config.coreId = tskNO_AFFINITY;
config.queueSize = 20;
config.maxPayloadSize = 128;
config.maxSubscriptions = 32;
config.maxWaiters = 8;
config.overflowPolicy = SignalOverflowPolicy::DropNewest;
config.defaultPostTimeoutMs = 0;
config.taskName = "signal-task";

bus.init(config);
```

## Queue Limits

`queueSize` is the maximum number of posted events waiting for dispatch. Each queue slot reserves up to `maxPayloadSize` bytes.

`maxSubscriptions` limits active subscriptions and the fixed dispatch-match capacity. `unsubscribe()` frees a slot for later reuse after active dispatch references drain.

`maxWaiters` limits tasks blocked in `waitFor()`. Set it to `0` to disable `waitFor()`; wait attempts then return `TooManyWaiters`.

Signal allocates queue slots, payload storage, dispatch storage, subscription records, waiter records, waiter semaphores, and the queue-space counting semaphore during `init()`. Movable Signal-owned storage follows `memory.allocation`; FreeRTOS synchronization control blocks remain internal through Strata. A failed `init()` rolls back partial storage so the object can be retried with a different config.

The bounded core guarantee applies to post, dispatch, wait registration, waiter completion, unsubscribe, diagnostics, and raw callback subscription after successful `init()`. Capturing lambda and `std::function` subscriptions may allocate during `subscribe()`.

During shutdown, storage is freed only after the dispatch task has stopped, active waiters have released their slots, and every blocking post operation has left the queue-space semaphore. If `end(timeoutMs)` returns `Timeout`, shutdown remains in progress and storage stays allocated.

## Overflow Policies

`DropNewest` rejects the new event when the queue is full.

`DropOldest` discards the oldest queued event, then accepts the new one.

`BlockCaller` waits for queue space up to the post timeout. `post()` uses `defaultPostTimeoutMs`; `postWithTimeout()` uses the supplied timeout. Internally, queue space is tracked with a counting semaphore whose tokens represent unreserved free queue slots.

A `BlockCaller` post from a Signal callback never waits. It can use an immediately available slot, but returns `Busy` when the queue is full so the dispatch task cannot deadlock itself.

## Memory and stack placement

Signal uses the same `Strata::MemoryPolicy` vocabulary as Worker and the other migrated ZekStack libraries.

- `memory.allocation` controls queue, payload, dispatch, subscription, and waiter backing storage.
- `memory.taskStack` controls the dispatcher task stack.
- The defaults are `Placement::Default` for ordinary storage and `Placement::PreferExternal` for the task stack.
- `PreferExternal` may fall back to internal memory.
- `RequireExternal` is strict and causes initialization to fail when the requested storage cannot be satisfied.
- `Internal` keeps the requested storage in internal RAM.

Stack size is in FreeRTOS bytes and must be at least 1024 bytes and aligned to `sizeof(StackType_t)`.

`SignalDiag` reports requested placement separately from observed regions. This is important because individual `PreferExternal` allocations may independently fall back to internal memory.
