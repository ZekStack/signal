#include <Arduino.h>
#include <Signal.h>

Signal signal;

enum class AppEvent : uint16_t {
	Burst,
};

void setup() {
	Serial.begin(115200);
	delay(200);

	SignalConfig config;
	config.queueSize = 2;
	config.overflowPolicy = SignalOverflowPolicy::DropNewest;

	SignalResult initResult = signal.init(config);
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	for (int i = 0; i < 5; i++) {
		SignalResult result = signal.post(AppEvent::Burst);
		Serial.printf("post %d: %s\n", i, signal.statusToString(result.status));
	}
}

void loop() {
	delay(1000);
}
