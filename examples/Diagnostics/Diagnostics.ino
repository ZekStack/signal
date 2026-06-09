#include <Arduino.h>
#include <Signal.h>

Signal bus;

enum class AppEvent : uint16_t {
	Tick,
};

void printDiagnostics() {
	SignalDiag diag = bus.getDiagnostics();
	Serial.printf("posted=%u\n", static_cast<unsigned>(diag.postedCount));
	Serial.printf("processed=%u\n", static_cast<unsigned>(diag.processedEventCount));
	Serial.printf("callbacks=%u\n", static_cast<unsigned>(diag.callbackInvokeCount));
	Serial.printf("dropped=%u\n", static_cast<unsigned>(diag.droppedCount));
	Serial.printf("rejected=%u\n", static_cast<unsigned>(diag.rejectedCount));
	Serial.printf("queue=%u/%u\n", static_cast<unsigned>(diag.queueUsed), static_cast<unsigned>(diag.queueSize));
	Serial.printf("subscriptions=%u\n", static_cast<unsigned>(diag.subscriptionCount));
	Serial.printf("waiters=%u\n", static_cast<unsigned>(diag.waiterCount));
}

void setup() {
	Serial.begin(115200);
	delay(200);

	SignalResult initResult = bus.init();
	if (!initResult) {
		Serial.println(initResult.message);
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
