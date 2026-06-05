#include <Arduino.h>
#include <Signal.h>

Signal signal;

enum class AppEvent : uint16_t {
	Ping,
};

void setup() {
	Serial.begin(115200);
	delay(200);

	SignalResult initResult = signal.init();
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	SignalSubResult sub = signal.subscribe(AppEvent::Ping, []() {
		Serial.println("this should only print before unsubscribe");
	});

	signal.post(AppEvent::Ping);
	delay(100);

	if (sub) {
		signal.unsubscribe(sub.id);
	}

	signal.post(AppEvent::Ping);
}

void loop() {
	delay(1000);
}
