# Getting Started

Signal provides a small event bus for Arduino ESP32 projects.

## Add Signal

PlatformIO:

```ini
lib_deps =
  https://github.com/ZekStack/signal.git

build_flags =
  -std=gnu++20
build_unflags =
  -std=gnu++11
```

Arduino IDE users can install the repository into `Arduino/libraries/Signal`.

## Create a bus

```cpp
#include <Signal.h>

Signal signal;

enum class AppEvent : uint16_t {
	Booted,
	Reading,
};
```

## Initialize

```cpp
SignalResult result = signal.init();
if (!result) {
	Serial.println(result.message.c_str());
	return;
}
```

## Subscribe and post

```cpp
signal.subscribe(AppEvent::Booted, []() {
	Serial.println("booted");
});

signal.post(AppEvent::Booted);
```

`post()` only queues the event. The callback runs later from the internal Signal task.

## Send a payload

```cpp
struct Reading {
	float temperature = 0.0f;
	uint32_t sequence = 0;
};

signal.subscribe<Reading>(AppEvent::Reading, [](const Reading &reading) {
	Serial.printf("temperature=%.2f\n", reading.temperature);
});

Reading reading;
reading.temperature = 23.5f;
reading.sequence = 1;
signal.post(AppEvent::Reading, reading);
```

Payloads must be trivially copyable and must fit in `SignalConfig::maxPayloadSize`.
