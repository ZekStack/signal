#include <Arduino.h>
#include <Signal.h>

Signal bus;

enum class AppEvent : uint16_t {
	Booted,
};

void setup() {
	Serial.begin(115200);
	delay(200);

	SignalResult initResult = bus.init();
	if (!initResult) {
		Serial.println(initResult.message);
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
