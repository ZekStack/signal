# Troubleshooting

## `signal is not initialized`

Call `init()` before subscribing, posting, or waiting. This response is also expected after shutdown has entered `Stopping`.

## `signal lifecycle transition in progress`

Another task is currently initializing or shutting down the same `Signal` instance. Concurrent lifecycle calls are serialized; retry after the current transition completes.

## Queue is full

Increase `queueSize`, reduce producer rate, keep callbacks short, or choose a different `SignalOverflowPolicy`.

## `Busy` from a callback post

With `BlockCaller`, Signal callbacks are not allowed to wait for queue space because callbacks run on the only task that drains the queue. The post succeeds when a slot is immediately available and otherwise returns `Busy`.

## Callback blocks other events

All subscriber callbacks run from the internal Signal task. Long callbacks delay dispatch of later events.

## Shutdown from callback

Do not call `end()` or destroy the `Signal` instance from a Signal callback. The destructor has a last-resort guard to avoid freeing memory still used by the Signal task, but that lifecycle pattern is unsupported.

## `end()` times out

Queued callbacks, active waiter tasks, or blocked producers may still be draining. Internal storage remains allocated after a timeout. New operations remain rejected, and a later `end()` call can complete cleanup.

## Handle subscription disappears immediately

`subscribeHandle()` returns a scoped handle. Store it for as long as the subscription should remain active. A temporary handle is destroyed at the end of the statement and unsubscribes immediately.

## Need allocation-free subscriptions

Use `subscribeRaw()` with a function pointer and context pointer. Lambda, `std::bind`, and `std::function` subscriptions are convenience APIs and may allocate during `subscribe()`.

## `Auto` reports an internal stack

`SignalStackType::Auto` first tries PSRAM when supported, then falls back to internal RAM if external task creation fails. Inspect `SignalDiag::requestedStackType` and `actualStackType` to see which path was used.
