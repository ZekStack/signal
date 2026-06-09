#include <Arduino.h>
#include <Signal.h>

Signal bus;
SignalSubscriptionHandle storedHandle;
SignalSubscriptionId resubscribeId = 0;

enum class AppEvent : uint16_t {
	Stress,
	Resubscribe,
	Ready,
	ShutdownWait,
	Scoped,
};

struct StressPayload {
	uint32_t producer = 0;
	uint32_t value = 0;
};

void stressPayloadCallback(void *, const void *payload, size_t payloadSize) {
	if (payload == nullptr || payloadSize != sizeof(StressPayload)) {
		return;
	}
	StressPayload item;
	memcpy(&item, payload, sizeof(item));
	Serial.printf(
	    "stress producer=%u value=%u\n",
	    static_cast<unsigned>(item.producer),
	    static_cast<unsigned>(item.value)
	);
	delay(25);
}

void scopedCallback(void *) {
	Serial.println("stored scoped handle callback");
}

void replacementCallback() {
	Serial.println("replacement callback");
}

void resubscribeCallback() {
	Serial.println("resubscribe callback");
	if (resubscribeId != 0) {
		bus.unsubscribe(resubscribeId);
		resubscribeId = 0;
		bus.subscribe(AppEvent::Resubscribe, replacementCallback);
	}
}

void producerTask(void *arg) {
	const uint32_t producer = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
	for (uint32_t i = 0; i < 8; i++) {
		StressPayload payload{producer, i};
		SignalResult result = bus.postWithTimeout(AppEvent::Stress, payload, 250);
		if (!result) {
			Serial.printf(
			    "producer %u post failed: %s\n",
			    static_cast<unsigned>(producer),
			    bus.statusToString(result.status)
			);
		}
		delay(5);
	}
	vTaskDelete(nullptr);
}

void readyWaiterTask(void *) {
	SignalResult result = bus.waitFor(AppEvent::Ready, 1000);
	Serial.printf("ready wait: %s\n", bus.statusToString(result.status));
	vTaskDelete(nullptr);
}

void timeoutWaiterTask(void *) {
	SignalResult result = bus.waitFor(AppEvent::ShutdownWait, 100);
	Serial.printf("timeout wait: %s\n", bus.statusToString(result.status));
	vTaskDelete(nullptr);
}

void shutdownWaiterTask(void *) {
	SignalResult result = bus.waitFor(AppEvent::ShutdownWait, 5000);
	Serial.printf("shutdown wait: %s\n", bus.statusToString(result.status));
	vTaskDelete(nullptr);
}

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

	SignalConfig config;
	config.queueSize = 3;
	config.maxPayloadSize = sizeof(StressPayload);
	config.maxSubscriptions = 8;
	config.maxWaiters = 3;
	config.overflowPolicy = SignalOverflowPolicy::BlockCaller;
	config.defaultPostTimeoutMs = 250;

	SignalResult initResult = bus.init(config);
	if (!initResult) {
		Serial.println(initResult.message);
		return;
	}

	bus.subscribeRaw(AppEvent::Stress, sizeof(StressPayload), stressPayloadCallback, nullptr);

	storedHandle = bus.subscribeRawHandle(AppEvent::Scoped, scopedCallback, nullptr);
	bus.subscribeRawHandle(AppEvent::Scoped, [](void *) {
		Serial.println("temporary handle callback should not run");
	});
	bus.post(AppEvent::Scoped);

	SignalSubResult sub = bus.subscribe(AppEvent::Resubscribe, resubscribeCallback);
	resubscribeId = sub.id;
	bus.post(AppEvent::Resubscribe);
	bus.post(AppEvent::Resubscribe);

	xTaskCreate(producerTask, "signal-prod-a", 4096, reinterpret_cast<void *>(1), 1, nullptr);
	xTaskCreate(producerTask, "signal-prod-b", 4096, reinterpret_cast<void *>(2), 1, nullptr);
	xTaskCreate(readyWaiterTask, "signal-ready", 4096, nullptr, 1, nullptr);
	xTaskCreate(timeoutWaiterTask, "signal-timeout", 4096, nullptr, 1, nullptr);
	xTaskCreate(shutdownWaiterTask, "signal-shutdown", 4096, nullptr, 1, nullptr);

	delay(100);
	bus.post(AppEvent::Ready);
	delay(1500);
	printDiagnostics();

	SignalResult endResult = bus.end(2000);
	Serial.printf("end: %s\n", bus.statusToString(endResult.status));
}

void loop() {
	delay(1000);
}
