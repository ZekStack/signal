#include <Arduino.h>
#include <Signal.h>

Signal signal;

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

	SignalResult initResult = signal.init();
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	signal.subscribe<SensorReading>(AppEvent::Reading, [](const SensorReading &reading) {
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

	signal.post(AppEvent::Reading, reading);
}

void loop() {
	delay(1000);
}
