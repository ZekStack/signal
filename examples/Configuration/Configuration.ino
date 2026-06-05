#include <Arduino.h>
#include <Signal.h>

Signal signal;

enum class AppEvent : uint16_t {
	Configured,
};

void setup() {
	Serial.begin(115200);
	delay(200);

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

	SignalResult initResult = signal.init(config);
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	signal.subscribe(AppEvent::Configured, []() {
		Serial.println("configured signal bus is running");
	});

	signal.post(AppEvent::Configured);
}

void loop() {
	delay(1000);
}
