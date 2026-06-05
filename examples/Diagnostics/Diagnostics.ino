#include <Arduino.h>
#include <Signal.h>

Signal bus;

enum class AppEvent : uint16_t {
	Tick,
};

void printDiagnostics() {
	SignalDiag diag = bus.getDiagnostics();
	Serial.printf("posted=%u\n", static_cast<unsigned>(diag.postedCount));
	Serial.printf("dispatched=%u\n", static_cast<unsigned>(diag.dispatchedCount));
	Serial.printf("dropped=%u\n", static_cast<unsigned>(diag.droppedCount));
	Serial.printf("queue=%u/%u\n", static_cast<unsigned>(diag.queueUsed), static_cast<unsigned>(diag.queueSize));
	Serial.printf("subscriptions=%u\n", static_cast<unsigned>(diag.subscriptionCount));
	Serial.printf("waiters=%u\n", static_cast<unsigned>(diag.waiterCount));
}

void setup() {
	Serial.begin(115200);
	delay(200);

	SignalResult initResult = bus.init();
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	bus.subscribe(AppEvent::Tick, []() {});
	bus.post(AppEvent::Tick);
	delay(100);
	printDiagnostics();
}

void loop() {
	delay(1000);
}
