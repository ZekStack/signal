# API

## Core Types

```cpp
using SignalEventId = uint32_t;
using SignalSubscriptionId = uint32_t;
```

Event IDs may be enum or integral values that fit in `uint32_t`.

## Results

`SignalResult` is returned by lifecycle, post, wait, and unsubscribe operations.

`SignalSubResult` extends `SignalResult` with a subscription ID.

```cpp
SignalSubResult result = signal.subscribe(AppEvent::Booted, []() {});
if (!result) {
	Serial.println(result.message.c_str());
}
```

## Lifecycle

```cpp
SignalResult init(const SignalConfig &config = SignalConfig());
SignalResult end(uint32_t timeoutMs = 5000);
```

`end()` stops accepting new posts, wakes current waiters with failure, drains queued events, and stops the dispatch task.

## Subscribe

```cpp
SignalSubResult subscribe(Event event, SignalCallback callback);
SignalSubResult subscribe<Payload>(Event event, Callback callback);
SignalResult unsubscribe(SignalSubscriptionId id);
```

No-payload subscribers match no-payload posts. Payload subscribers match by event ID and exact payload size.

Callbacks run from the internal Signal task. Keep callbacks short and avoid blocking forever.

## Post

```cpp
SignalResult post(Event event);
SignalResult post(Event event, const Payload &payload);
SignalResult postWithTimeout(Event event, uint32_t timeoutMs);
SignalResult postWithTimeout(Event event, const Payload &payload, uint32_t timeoutMs);
```

`post()` only enqueues accepted events. It does not run callbacks inline.

## Wait

```cpp
SignalResult waitFor(Event event, uint32_t timeoutMs);
SignalResult waitFor(Event event, Payload &out, uint32_t timeoutMs);
```

`waitFor()` registers a future-only waiter. It does not inspect old events and does not consume subscriber delivery.

Calling `waitFor()` from the internal Signal task returns `InvalidArgument`.

## Diagnostics

```cpp
SignalDiag diag = signal.getDiagnostics();
```

Diagnostics include posted, dispatched, dropped, queue usage, subscription count, waiter count, dispatch errors, and task stack high-water mark.
