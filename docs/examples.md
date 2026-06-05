# Examples

## Basic

Minimal initialization, no-payload subscription, posting, and shutdown.

## Payloads

Shows a trivially copyable payload struct and typed subscription.

## WaitFor

Starts a FreeRTOS task that blocks until a future event is posted.

## Unsubscribe

Shows that an unsubscribed callback does not receive later events.

## OverflowPolicies

Configures a small queue and demonstrates post results under queue pressure.

## Diagnostics

Prints `SignalDiag` counters and configured limits.

## BindableCallbacks

Uses `std::bind` to subscribe a private class method.

## Configuration

Shows stack, queue, payload, subscription, waiter, overflow, and task-name configuration.
