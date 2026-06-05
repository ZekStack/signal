#include <Arduino.h>
#include <Signal.h>

Signal bus;

enum class AppEvent : uint16_t {
	Ready,
};

void waiterTask(void *) {
	SignalResult result = bus.waitFor(AppEvent::Ready, 5000);
	if (result) {
		Serial.println("waiter received Ready");
	} else {
		Serial.println(result.message.c_str());
	}
	vTaskDelete(nullptr);
}

void setup() {
	Serial.begin(115200);
	delay(200);

	SignalResult initResult = bus.init();
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	xTaskCreate(waiterTask, "signal-waiter", 4096, nullptr, 1, nullptr);
	delay(250);
	bus.post(AppEvent::Ready);
}

void loop() {
	delay(1000);
}
