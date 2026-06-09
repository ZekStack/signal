#include <Arduino.h>
#include <Signal.h>

Signal bus;

enum class AppEvent : uint16_t {
	Ping,
};

void setup() {
	Serial.begin(115200);
	delay(200);

	SignalResult initResult = bus.init();
	if (!initResult) {
		Serial.println(initResult.message);
		return;
	}

	SignalSubResult sub = bus.subscribe(AppEvent::Ping, []() {
		Serial.println("this should only print before unsubscribe");
	});

	bus.post(AppEvent::Ping);
	delay(100);

	if (sub) {
		bus.unsubscribe(sub.id);
	}

	bus.post(AppEvent::Ping);
}

void loop() {
	delay(1000);
}
