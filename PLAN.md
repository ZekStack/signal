## Signal

An event bus and publish-subscribe library for ESP32.

Signal provides a clean way to decouple modules by allowing them to publish and subscribe to typed events without direct dependencies between components.

---

### Rules

*   No exceptions, no throw
*   Embedded friendly
*   Thread safe
*   waitFor() only waits for future events. It does not consume already dispatched events from the global bus history.
*   Payload types must be trivially copyable or safely byte-copyable.  
    Do not use std::string, std::vector, heap pointers, references, or objects with destructors inside payloads.
*   post() only enqueues events.
*   Subscriber callbacks are executed from the internal Signal task.
*   waitFor() blocks the caller task until a future matching event is posted or if timeout
*   waitFor() does not steal events from subscribers.  
    A posted event is broadcast to subscribers and also wakes matching waiters.
*   A posted event wakes all matching waiters.  
    Each waiter receives its own copied payload.

---

### Example usage

```src
#include <Signal.h>

Signal bus;

enum class AppEvent : uint16_t {
    TestAppEvent,
    PayloadAppEvent,
    UnSubEvent,
};

struct SomePayload {
    int test = 0;
    char message[64] = "Hello world!";
};

int runCount = 0;

void setup() {
   	SignalConfig config;
	config.stackSizeBytes = 4096;
	config.coreId = tskNO_AFFINITY;
	config.priority = 1;
	config.stackType = SignalStackType::Auto;

	config.queueSize = 20;
	config.maxPayloadSize = 128;
	config.maxSubscriptions = 32;
	config.maxWaiters = 8;

	config.overflowPolicy = SignalOverflowPolicy::DropNewest;
	config.defaultPostTimeoutMs = 0;

    SignalResult initResult = bus.init(config);
    if (!initResult) {
        Serial.println(initResult.message);
        return;
    }

    SignalSubResult subResult = bus.subscribe(AppEvent::TestAppEvent, []() {
        Serial.println("A test app event happened!");
    });

    if (!subResult) {
        Serial.println(subResult.message);
        return;
    }

    bus.subscribe<SomePayload>(AppEvent::PayloadAppEvent, [](const SomePayload& payload) {
        Serial.printf(
            "Payload event happened. test=%d, message=%s\n",
            payload.test,
            payload.message
        );
    });
    
    SignalSubResult unSubResult = bus.subscribe(AppEvent::UnSubEvent, []() {
        Serial.println("An unsub app event happened!");
    });
    // SignalSubscriptionId
    if (unSubResult) {
    	SignalResult unsubResult = bus.unsubscribe(unSubResult.id);
		if (!unsubResult) {
    		Serial.println(unsubResult.message);
		}
	}

    emitter();
}

void emitter() {
    SignalResult postResultOne = bus.post(AppEvent::TestAppEvent);
    if (!postResultOne) {
        Serial.println(postResultOne.message);
    }

    SomePayload payload;
    payload.test = 123;
    snprintf(payload.message, sizeof(payload.message), "Hello from emitter");

    SignalResult postResultTwo = bus.post(AppEvent::PayloadAppEvent, payload);
    if (!postResultTwo) {
        Serial.println(postResultTwo.message);
    }
}

void loop() {
    SomePayload data;

    SignalResult result = bus.waitFor(AppEvent::PayloadAppEvent, data, portMAX_DELAY);
    if (result) {
        Serial.printf(
            "Payload received from loop. test=%d, message=%s\n",
            data.test,
            data.message
        );
    }
    
    SignalResult resultTwo = bus.waitFor(AppEvent::TestAppEvent, portMAX_DELAY);
}

/*
	SignalDiag diag = bus.getDiagnostics();

	diag.postedCount;
	diag.dispatchedCount;
	diag.droppedCount;
	diag.queueSize;
	diag.queueUsed;
	diag.subscriptionCount;
	diag.waiterCount;
	diag.dispatchErrorCount;
	
	enum class SignalOverflowPolicy {
    	DropNewest,
    	DropOldest,
    	BlockCaller
	};
*/
```

## Other methods

```
bus.post(AppEvent::TestAppEvent);
bus.postWithTimeout(AppEvent::TestAppEvent, 100); // wait up to 100ms for queue space

bus.post(AppEvent::PayloadAppEvent, payload);
bus.postWithTimeout(AppEvent::PayloadAppEvent, payload, 100);

/*
Where timeout means:

How long post() may wait for queue space.

Default can be non-blocking:

Default post() does not block if the queue is full.
*/
```