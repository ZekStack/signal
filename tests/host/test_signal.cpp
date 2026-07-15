#include <Signal.h>

#include "fake_freertos.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

enum class Event : uint16_t {
	Trigger,
	Queued,
	Third,
	Payload,
	Wait,
	ShutdownWait,
};

void fail(const char *expression, const char *file, int line) {
	std::cerr << file << ':' << line << ": assertion failed: " << expression << '\n';
	std::exit(1);
}

#define CHECK(expression)                                                                          \
	do {                                                                                           \
		if (!(expression)) {                                                                       \
			fail(#expression, __FILE__, __LINE__);                                                  \
		}                                                                                          \
	} while (false)

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 1000ms) {
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline) {
		if (predicate()) {
			return true;
		}
		std::this_thread::sleep_for(1ms);
	}
	return predicate();
}

void testConcurrentInitIsSerialized() {
	fake_freertos::reset();
	fake_freertos::setTaskCreateDelayMs(50);
	Signal bus;
	SignalResult first;
	SignalResult second;

	std::thread firstThread([&]() { first = bus.init(); });
	std::this_thread::sleep_for(5ms);
	std::thread secondThread([&]() { second = bus.init(); });
	firstThread.join();
	secondThread.join();

	CHECK(first);
	CHECK(!second);
	CHECK(second.status == SignalStatus::Busy);
	CHECK(fake_freertos::taskCreateCount() == 1);
	CHECK(bus.end(1000));
}

void testFailedInitCanBeRetried() {
	fake_freertos::reset();
	fake_freertos::failNextTaskCreates(1);
	Signal bus;
	SignalResult failed = bus.init();
	CHECK(!failed);
	CHECK(failed.status == SignalStatus::TaskCreateFailed);
	CHECK(bus.init());
	CHECK(bus.end(1000));
}

void testPsramAutoFallback() {
	fake_freertos::reset();
	fake_freertos::setPsramBytes(1024 * 1024);
	fake_freertos::failNextCapsTaskCreates(1);
	Signal bus;
	SignalConfig config;
	config.stackType = SignalStackType::Auto;
	CHECK(bus.init(config));
	SignalDiag diag = bus.getDiagnostics();
	CHECK(diag.requestedStackType == SignalStackType::Auto);
	CHECK(diag.actualStackType == SignalStackType::Internal);
	CHECK(fake_freertos::capsTaskCreateCount() == 1);
	CHECK(fake_freertos::taskCreateCount() == 1);
	CHECK(bus.end(1000));
}

void testExplicitPsramDoesNotFallback() {
	fake_freertos::reset();
	fake_freertos::setPsramBytes(1024 * 1024);
	fake_freertos::failNextCapsTaskCreates(1);
	Signal bus;
	SignalConfig config;
	config.stackType = SignalStackType::Psram;
	SignalResult result = bus.init(config);
	CHECK(!result);
	CHECK(result.status == SignalStatus::TaskCreateFailed);
	CHECK(fake_freertos::capsTaskCreateCount() == 1);
	CHECK(fake_freertos::taskCreateCount() == 0);
}

void testCallbackBlockingPostReturnsBusy() {
	fake_freertos::reset();
	Signal bus;
	SignalConfig config;
	config.queueSize = 1;
	config.overflowPolicy = SignalOverflowPolicy::BlockCaller;
	config.defaultPostTimeoutMs = portMAX_DELAY;
	CHECK(bus.init(config));

	std::promise<SignalStatus> statusPromise;
	auto statusFuture = statusPromise.get_future();
	CHECK(bus.subscribe(Event::Trigger, [&]() {
		CHECK(bus.post(Event::Queued));
		statusPromise.set_value(bus.postWithTimeout(Event::Third, portMAX_DELAY).status);
	}));
	CHECK(bus.post(Event::Trigger));
	CHECK(statusFuture.wait_for(1s) == std::future_status::ready);
	CHECK(statusFuture.get() == SignalStatus::Busy);
	CHECK(bus.end(1000));
}

void testBlockedProducerDrainsBeforeShutdownCleanup() {
	fake_freertos::reset();
	Signal bus;
	SignalConfig config;
	config.queueSize = 1;
	config.overflowPolicy = SignalOverflowPolicy::BlockCaller;
	CHECK(bus.init(config));

	std::atomic<bool> callbackEntered{false};
	std::atomic<bool> releaseCallback{false};
	CHECK(bus.subscribe(Event::Trigger, [&]() {
		callbackEntered.store(true);
		while (!releaseCallback.load()) {
			std::this_thread::sleep_for(1ms);
		}
	}));
	CHECK(bus.post(Event::Trigger));
	CHECK(waitUntil([&]() { return callbackEntered.load(); }));
	CHECK(bus.post(Event::Queued));

	std::promise<SignalStatus> producerStatusPromise;
	auto producerStatusFuture = producerStatusPromise.get_future();
	std::thread producer([&]() {
		producerStatusPromise.set_value(bus.postWithTimeout(Event::Third, portMAX_DELAY).status);
	});
	std::this_thread::sleep_for(10ms);

	std::promise<SignalStatus> endStatusPromise;
	auto endStatusFuture = endStatusPromise.get_future();
	std::thread shutdown([&]() { endStatusPromise.set_value(bus.end(2000).status); });
	std::this_thread::sleep_for(10ms);
	releaseCallback.store(true);

	producer.join();
	shutdown.join();
	CHECK(producerStatusFuture.get() == SignalStatus::NotInitialized);
	CHECK(endStatusFuture.get() == SignalStatus::Ok);
}

void testEndTimeoutCanBeCompletedLater() {
	fake_freertos::reset();
	Signal bus;
	CHECK(bus.init());
	std::atomic<bool> callbackEntered{false};
	std::atomic<bool> releaseCallback{false};
	CHECK(bus.subscribe(Event::Trigger, [&]() {
		callbackEntered.store(true);
		while (!releaseCallback.load()) {
			std::this_thread::sleep_for(1ms);
		}
	}));
	CHECK(bus.post(Event::Trigger));
	CHECK(waitUntil([&]() { return callbackEntered.load(); }));
	SignalResult timedOut = bus.end(10);
	CHECK(!timedOut);
	CHECK(timedOut.status == SignalStatus::Timeout);
	CHECK(bus.post(Event::Queued).status == SignalStatus::NotInitialized);
	releaseCallback.store(true);
	CHECK(bus.end(1000));
}

void testConcurrentEndCallsComplete() {
	fake_freertos::reset();
	Signal bus;
	CHECK(bus.init());
	std::promise<SignalStatus> firstPromise;
	std::promise<SignalStatus> secondPromise;
	auto firstFuture = firstPromise.get_future();
	auto secondFuture = secondPromise.get_future();
	std::thread first([&]() { firstPromise.set_value(bus.end(1000).status); });
	std::thread second([&]() { secondPromise.set_value(bus.end(1000).status); });
	first.join();
	second.join();
	CHECK(firstFuture.get() == SignalStatus::Ok);
	CHECK(secondFuture.get() == SignalStatus::Ok);
}

void testWaitersBroadcastAndShutdownWake() {
	fake_freertos::reset();
	Signal bus;
	SignalConfig config;
	config.maxWaiters = 3;
	CHECK(bus.init(config));

	auto waiterA = std::async(std::launch::async, [&]() { return bus.waitFor(Event::Wait, 1000); });
	auto waiterB = std::async(std::launch::async, [&]() { return bus.waitFor(Event::Wait, 1000); });
	CHECK(waitUntil([&]() { return bus.getDiagnostics().waiterCount == 2; }));
	CHECK(bus.post(Event::Wait));
	CHECK(waiterA.get());
	CHECK(waiterB.get());

	auto shutdownWaiter =
	    std::async(std::launch::async, [&]() { return bus.waitFor(Event::ShutdownWait, 5000); });
	CHECK(waitUntil([&]() { return bus.getDiagnostics().waiterCount == 1; }));
	CHECK(bus.end(1000));
	SignalResult shutdownResult = shutdownWaiter.get();
	CHECK(!shutdownResult);
	CHECK(shutdownResult.status == SignalStatus::NotInitialized);
}

void testDropOldestOrdering() {
	fake_freertos::reset();
	Signal bus;
	SignalConfig config;
	config.queueSize = 2;
	config.maxPayloadSize = sizeof(uint32_t);
	config.overflowPolicy = SignalOverflowPolicy::DropOldest;
	CHECK(bus.init(config));

	std::atomic<bool> blockerEntered{false};
	std::atomic<bool> releaseBlocker{false};
	CHECK(bus.subscribe(Event::Trigger, [&]() {
		blockerEntered.store(true);
		while (!releaseBlocker.load()) {
			std::this_thread::sleep_for(1ms);
		}
	}));

	std::mutex valuesMutex;
	std::vector<uint32_t> values;
	CHECK(bus.subscribe<uint32_t>(Event::Payload, [&](const uint32_t &value) {
		std::lock_guard lock(valuesMutex);
		values.push_back(value);
	}));

	CHECK(bus.post(Event::Trigger));
	CHECK(waitUntil([&]() { return blockerEntered.load(); }));
	CHECK(bus.post(Event::Payload, uint32_t{1}));
	CHECK(bus.post(Event::Payload, uint32_t{2}));
	CHECK(bus.post(Event::Payload, uint32_t{3}));
	releaseBlocker.store(true);
	CHECK(waitUntil([&]() {
		std::lock_guard lock(valuesMutex);
		return values.size() == 2;
	}));
	{
		std::lock_guard lock(valuesMutex);
		CHECK(values[0] == 2);
		CHECK(values[1] == 3);
	}
	SignalDiag diag = bus.getDiagnostics();
	CHECK(diag.droppedCount == 1);
	CHECK(bus.end(1000));
}

struct NoDefaultPayload {
	int value;
	NoDefaultPayload() = delete;
	explicit constexpr NoDefaultPayload(int newValue) : value(newValue) {
	}
};
static_assert(std::is_trivially_copyable_v<NoDefaultPayload>);

void testTypedPayloadDoesNotRequireDefaultConstructor() {
	fake_freertos::reset();
	Signal bus;
	CHECK(bus.init());
	std::promise<int> valuePromise;
	auto valueFuture = valuePromise.get_future();
	CHECK(bus.subscribe<NoDefaultPayload>(Event::Payload, [&](const NoDefaultPayload &payload) {
		valuePromise.set_value(payload.value);
	}));
	CHECK(bus.post(Event::Payload, NoDefaultPayload{42}));
	CHECK(valueFuture.wait_for(1s) == std::future_status::ready);
	CHECK(valueFuture.get() == 42);
	CHECK(bus.end(1000));
}

void testLiveStackDiagnostics() {
	fake_freertos::reset();
	Signal bus;
	CHECK(bus.init());
	CHECK(bus.post(Event::Trigger));
	CHECK(waitUntil([&]() { return bus.getDiagnostics().processedEventCount == 1; }));
	CHECK(bus.getDiagnostics().stackHighWaterMarkBytes > 0);
	CHECK(bus.end(1000));
}
} // namespace

int main() {
	testConcurrentInitIsSerialized();
	testFailedInitCanBeRetried();
	testPsramAutoFallback();
	testExplicitPsramDoesNotFallback();
	testCallbackBlockingPostReturnsBusy();
	testBlockedProducerDrainsBeforeShutdownCleanup();
	testEndTimeoutCanBeCompletedLater();
	testConcurrentEndCallsComplete();
	testWaitersBroadcastAndShutdownWake();
	testDropOldestOrdering();
	testTypedPayloadDoesNotRequireDefaultConstructor();
	testLiveStackDiagnostics();
	std::cout << "Signal host tests passed\n";
	return 0;
}
