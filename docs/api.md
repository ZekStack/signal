# API

## Core Types

```cpp
namespace zek::signal {
using SignalEventId = uint32_t;
using SignalSubscriptionId = uint32_t;
}
```

Event IDs may be enum or integral values that fit in `uint32_t`.

Signal types live in `zek::signal`. Global aliases such as `Signal`, `SignalResult`, and `SignalConfig` are enabled by default unless `ZEK_SIGNAL_DISABLE_GLOBAL_ALIASES` is defined.

## Results

`SignalResult` is returned by lifecycle, post, wait, and unsubscribe operations.

`SignalSubResult` extends `SignalResult` with a subscription ID.

```cpp
SignalSubResult result = bus.subscribe(AppEvent::Booted, []() {});
if (!result) {
	Serial.println(result.message);
}
```

## Lifecycle

```cpp
SignalResult init(const SignalConfig &config = SignalConfig());
SignalResult end(uint32_t timeoutMs = 5000);
```

Lifecycle transitions are serialized internally:

```text
Stopped -> Initializing -> Running -> Stopping -> Stopped
```

A concurrent `init()` during initialization or shutdown returns `Busy`. Calling `init()` while running returns `AlreadyInitialized`.

`end()` stops accepting new posts, subscriptions, and waits; wakes current waiters with failure; drains queued events; stops the dispatch task; waits for active blocking post operations and waiter calls to leave internal storage; then frees that storage.

If `end(timeoutMs)` returns `Timeout`, lifecycle remains `Stopping`, storage remains allocated, and a later `end()` call can finish cleanup.

Calling `end()` from the internal Signal task returns `InvalidArgument`.

Destroying a `Signal` instance from one of its callbacks is unsupported. As a last-resort safety guard, the destructor avoids freeing the internal implementation when destruction happens from the Signal task.

## Subscribe

```cpp
SignalSubResult subscribe(Event event, SignalCallback callback);
SignalSubResult subscribe<Payload>(Event event, Callback callback);
SignalSubResult subscribeRaw(Event event, SignalRawCallback callback, void *context = nullptr);
SignalSubResult subscribeRaw(
    Event event,
    size_t payloadSize,
    SignalRawPayloadCallback callback,
    void *context = nullptr
);
SignalSubscriptionHandle subscribeHandle(Event event, SignalCallback callback);
SignalSubscriptionHandle subscribeRawHandle(Event event, SignalRawCallback callback, void *context = nullptr);
SignalResult unsubscribe(SignalSubscriptionId id);
```

No-payload subscribers match no-payload posts. Payload subscribers match by event ID and exact payload size.

Callbacks run from the internal Signal task. Keep callbacks short and avoid blocking forever.

`unsubscribe()` prevents a not-yet-started callback from being invoked. A callback already executing is allowed to finish.

Signal has two callback paths:

* Bounded callbacks use function pointers plus an optional context pointer. This path does not allocate after `init()`.
* Convenience callbacks use lambda, `std::bind`, or `std::function`. These may allocate during `subscribe()`, but dispatch does not copy the callback.

`SignalSubscriptionHandle` is move-only and unsubscribes in its destructor. Store the returned handle; an unused temporary unsubscribes immediately. The `Signal` instance must outlive active handles.

## Post

```cpp
SignalResult post(Event event);
SignalResult post(Event event, const Payload &payload);
SignalResult postWithTimeout(Event event, uint32_t timeoutMs);
SignalResult postWithTimeout(Event event, const Payload &payload, uint32_t timeoutMs);
```

`post()` only enqueues accepted events. It does not run callbacks inline.

When `BlockCaller` is configured, posts from ordinary tasks wait for a queue-space reservation up to the configured timeout. A post from the Signal task itself never waits because that would deadlock the only dispatch task. It attempts an immediate reservation and returns `Busy` when the queue is full.

Blocking post operations are registered before they release the internal mutex. Shutdown does not delete queue or semaphore storage until every registered post operation has returned.

## Wait

```cpp
SignalResult waitFor(Event event, uint32_t timeoutMs);
SignalResult waitFor(Event event, Payload &out, uint32_t timeoutMs);
```

`waitFor()` registers a future-only waiter. It does not inspect old events and does not consume subscriber delivery.

Calling `waitFor()` from the internal Signal task returns `InvalidArgument`.

If `SignalConfig::maxWaiters` is `0`, `waitFor()` is disabled and returns `TooManyWaiters`.

## Payloads

Payload types must be trivially copyable. Typed callback reconstruction uses C++20 byte-wise reconstruction and does not require the payload type to be default-constructible.

## Diagnostics

```cpp
SignalDiag diag = bus.getDiagnostics();
```

Diagnostics include posted, processed, callback, dropped, rejected, queue usage, subscription count, waiter count, dispatch errors, and task stack high-water mark.

Use `processedEventCount` for dequeued events and `callbackInvokeCount` for actual callback calls. `dispatchedCount` remains as a compatibility alias for processed events.

`stackHighWaterMarkBytes` is the minimum observed remaining stack space for the running Signal task. It is sampled while the task is active and again during shutdown.
