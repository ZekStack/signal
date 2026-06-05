#include <Arduino.h>
#include <Signal.h>

Signal signal;

enum class AppEvent : uint16_t {
	Booted,
};

void setup() {
	Serial.begin(115200);
	delay(200);

	SignalResult initResult = signal.init();
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	signal.subscribe(AppEvent::Booted, []() {
		Serial.println("boot event received");
	});

	signal.post(AppEvent::Booted);
}

void loop() {
	delay(1000);
}
