# Signal

Signal is an event bus and publish-subscribe library for ESP32.

Signal helps Arduino ESP32 applications decouple modules with typed events, bounded queues, task-side callback dispatch, future-only waits, and runtime diagnostics. It is designed for products that need predictable event flow without direct dependencies between components.

[![CI](https://github.com/ZekStack/signal/actions/workflows/ci.yml/badge.svg)](https://github.com/ZekStack/signal/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ZekStack/signal?sort=semver)](https://github.com/ZekStack/signal/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Why use Signal?

* **Typed events** - publish enum or integral event IDs with optional trivially copyable payloads.
* **Bounded memory** - queue size, payload bytes, subscriptions, and waiters are configured up front.
* **Task-side callbacks** - subscriber callbacks run from the internal Signal task.
* **Future-only waits** - `waitFor()` wakes on future matching posts and does not consume subscriber events.
* **Production-minded** - result-based errors, diagnostics, thread-safe internals, and no exceptions.

## Install

### PlatformIO

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino

lib_deps =
  https://github.com/ZekStack/signal.git

build_flags =
  -std=gnu++20
build_unflags =
  -std=gnu++11
```

### Arduino IDE

Signal is not published to Arduino Library Manager yet.

Install it by downloading the repository ZIP or cloning it into your Arduino libraries folder.

```txt
Arduino/libraries/Signal
```

## Quick start

```cpp
#include <Arduino.h>
#include <Signal.h>

Signal bus;

enum class AppEvent : uint16_t {
	Booted,
};

void setup() {
	Serial.begin(115200);

	SignalResult initResult = bus.init();
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	bus.subscribe(AppEvent::Booted, []() {
		Serial.println("boot event received");
	});

	bus.post(AppEvent::Booted);
}

void loop() {
	delay(1000);
}
```

## Important notes

> [!IMPORTANT]
> `post()` only enqueues events. Subscriber callbacks run later from the internal Signal task.

* Payloads must be trivially copyable and fit inside `maxPayloadSize`.
* Do not put `std::string`, `std::vector`, heap pointers, references, or destructor-owned resources inside payloads.
* `waitFor()` only waits for future posts; it does not read from a global event history.
* A posted event wakes all matching waiters and is also delivered to subscribers.
* Stack sizes are FreeRTOS byte sizes on ESP32 and must be at least 1024 bytes.
* `SignalStackType::Auto` prefers PSRAM task stacks when supported and falls back to internal RAM.

## Examples

| Example | Description |
| --- | --- |
| `Basic` | Minimal init, subscribe, post, and shutdown. |
| `Payloads` | Trivially copyable payload publish-subscribe. |
| `WaitFor` | Blocking until a future event or timeout. |
| `Unsubscribe` | Removing a subscription. |
| `OverflowPolicies` | Queue limits and overflow behavior. |
| `Diagnostics` | Runtime counters and configured limits. |
| `BindableCallbacks` | `std::bind` with private class methods. |
| `Configuration` | Stack, queue, payload, subscription, and waiter limits. |

Start with:

```txt
examples/Basic
```

## Documentation

Detailed documentation is available in the `docs/` folder.

| Document | Description |
| --- | --- |
| [`docs/getting-started.md`](docs/getting-started.md) | Step-by-step setup and first event flow. |
| [`docs/configuration.md`](docs/configuration.md) | Config options, queue limits, waiters, and stack behavior. |
| [`docs/api.md`](docs/api.md) | Public classes, result types, callbacks, and diagnostics. |
| [`docs/examples.md`](docs/examples.md) | Explanation of all included examples. |
| [`docs/troubleshooting.md`](docs/troubleshooting.md) | Common issues and solutions. |

## API overview

```cpp
Signal bus;
bus.init();

SignalSubResult sub = bus.subscribe(AppEvent::Booted, []() {});
bus.unsubscribe(sub.id);

bus.post(AppEvent::Booted);
bus.postWithTimeout(AppEvent::Booted, 100);

SignalDiag diag = bus.getDiagnostics();
```

For the full API, see [`docs/api.md`](docs/api.md).

## Compatibility

| Item | Support |
| --- | --- |
| Framework | Arduino ESP32 |
| Platform | `espressif32` |
| Language | C++20 |
| Filesystem | none |
| PSRAM | Optional for task stacks when ESP-IDF support is available |
| Dependencies | none |
| Exceptions | Not used |
| Status | Early-stage `0.0.1` |

## License

MIT - see [`LICENSE.md`](LICENSE.md).

## ZekStack

Part of the ZekStack ESP32 library stack.
