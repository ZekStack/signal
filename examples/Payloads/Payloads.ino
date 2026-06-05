#include <Arduino.h>
#include <Signal.h>

Signal bus;

enum class AppEvent : uint16_t {
	Reading,
};

struct SensorReading {
	uint32_t sequence = 0;
	float temperature = 0.0f;
	char source[16] = {};
};

void setup() {
	Serial.begin(115200);
	delay(200);

	SignalResult initResult = bus.init();
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	bus.subscribe<SensorReading>(AppEvent::Reading, [](const SensorReading &reading) {
		Serial.printf(
		    "reading seq=%u source=%s temperature=%.2f\n",
		    static_cast<unsigned>(reading.sequence),
		    reading.source,
		    reading.temperature
		);
	});

	SensorReading reading;
	reading.sequence = 1;
	reading.temperature = 23.5f;
	snprintf(reading.source, sizeof(reading.source), "kitchen");

	bus.post(AppEvent::Reading, reading);
}

void loop() {
	delay(1000);
}
