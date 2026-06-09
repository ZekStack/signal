# Configuration

`SignalConfig` controls task behavior and memory limits.

```cpp
SignalConfig config;
config.stackSizeBytes = 4096;
config.priority = 1;
config.coreId = tskNO_AFFINITY;
config.stackType = SignalStackType::Auto;
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

`maxWaiters` limits tasks blocked in `waitFor()`.

Signal allocates queue slots, payload storage, dispatch storage, subscription records, waiter records, waiter semaphores, and the queue-space counting semaphore during `init()`. A failed `init()` rolls back partial storage so the object can be retried with a different config.

The bounded core guarantee applies to post, dispatch, wait registration, waiter completion, unsubscribe, diagnostics, and raw callback subscription after successful `init()`. Capturing lambda and `std::function` subscriptions may allocate during `subscribe()`.

## Overflow Policies

`DropNewest` rejects the new event when the queue is full.

`DropOldest` discards the oldest queued event, then accepts the new one.

`BlockCaller` waits for queue space up to the post timeout. `post()` uses `defaultPostTimeoutMs`; `postWithTimeout()` uses the supplied timeout. Internally, queue space is tracked with a counting semaphore whose count matches free queue slots.

## Stack Behavior

Stack size is in FreeRTOS bytes, matching ESP-IDF-flavored task APIs used across ZekStack.

`SignalStackType::Auto` prefers PSRAM task stacks when supported by the platform and falls back to internal RAM.

`SignalStackType::Psram` fails initialization if PSRAM task stacks are unavailable.
