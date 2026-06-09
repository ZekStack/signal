# Troubleshooting

## `signal is not initialized`

Call `init()` before subscribing, posting, or waiting. This can also happen while `end()` is in progress.

## `payload is too large`

Increase `SignalConfig::maxPayloadSize` or reduce the payload struct size.

## Payload callback does not run

Payload subscribers match by event ID and exact payload size. Check that the subscriber and post use the same payload type.

## `waitFor()` times out

`waitFor()` only waits for future events. A matching event posted before `waitFor()` starts is not consumed from history.

## Queue full

Increase `queueSize`, reduce producer rate, keep callbacks short, or choose a different `SignalOverflowPolicy`.

## Callback blocks other events

All subscriber callbacks run from the internal Signal task. Long callbacks delay dispatch of later events.

## Handle subscription disappears immediately

`subscribeHandle()` returns a scoped handle. Store it for as long as the subscription should remain active. A temporary handle is destroyed at the end of the statement and unsubscribes immediately.

## Need allocation-free subscriptions

Use `subscribeRaw()` with a function pointer and context pointer. Lambda, `std::bind`, and `std::function` subscriptions are convenience APIs and may allocate during `subscribe()`.
