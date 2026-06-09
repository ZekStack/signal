#include "Signal.h"

#include "internal/SignalMutex.h"
#include "internal/SignalTaskSupport.h"

#include <cstring>
#include <new>

#include <freertos/semphr.h>

namespace zek::signal {
namespace {
constexpr SignalSubscriptionId kInvalidSubscriptionId = 0;

bool timeoutElapsed(uint32_t startedMs, uint32_t timeoutMs) {
	if (timeoutMs == portMAX_DELAY) {
		return false;
	}
	return millis() - startedMs >= timeoutMs;
}

TickType_t timeoutToTicks(uint32_t timeoutMs) {
	if (timeoutMs == portMAX_DELAY) {
		return portMAX_DELAY;
	}
	return pdMS_TO_TICKS(timeoutMs);
}

SignalResult allocationFailure() {
	return SignalResult::failure(SignalStatus::OutOfMemory, "signal allocation failed");
}

SignalSubResult subscriptionAllocationFailure() {
	return SignalSubResult::failure(SignalStatus::OutOfMemory, "signal allocation failed");
}
} // namespace

struct SignalQueuedEvent {
	SignalEventId eventId = 0;
	size_t payloadSize = 0;
	uint64_t sequence = 0;
	uint8_t *payload = nullptr;
};

struct SignalDispatchEvent {
	SignalEventId eventId = 0;
	size_t payloadSize = 0;
	uint64_t sequence = 0;
	const uint8_t *payload = nullptr;
};

enum class SignalCallbackKind : uint8_t {
	None,
	Raw,
	RawPayload,
	Function,
};

struct SignalSubscriptionRecord {
	SignalSubscriptionId id = kInvalidSubscriptionId;
	uint32_t generation = 0;
	bool active = false;
	bool pendingCleanup = false;
	uint16_t dispatchRefs = 0;
	SignalEventId eventId = 0;
	size_t payloadSize = 0;
	SignalCallbackKind kind = SignalCallbackKind::None;
	SignalRawCallback rawCallback = nullptr;
	SignalRawPayloadCallback rawPayloadCallback = nullptr;
	void *context = nullptr;
	std::function<void(const void *, size_t)> functionCallback;

	bool available() const {
		return !active && dispatchRefs == 0;
	}

	void clearCallback() {
		rawCallback = nullptr;
		rawPayloadCallback = nullptr;
		context = nullptr;
		functionCallback = nullptr;
		kind = SignalCallbackKind::None;
	}
};

struct SignalWaiterRecord {
	SignalEventId eventId = 0;
	size_t payloadSize = 0;
	void *payloadOut = nullptr;
	SemaphoreHandle_t done = nullptr;
	bool inUse = false;
	bool completed = false;
	SignalStatus status = SignalStatus::Timeout;
	const char *message = "signal wait timed out";
};

struct SignalDispatchMatch {
	size_t index = 0;
	SignalSubscriptionId id = kInvalidSubscriptionId;
	uint32_t generation = 0;
};

struct SignalImpl {
	SignalConfig config{};
	SignalMutex mutex;
	SignalQueuedEvent *queue = nullptr;
	uint8_t *queuePayloadStorage = nullptr;
	uint8_t *dispatchPayload = nullptr;
	SignalDispatchMatch *dispatchMatches = nullptr;
	size_t queueHead = 0;
	size_t queueCount = 0;
	SignalSubscriptionRecord *subscriptions = nullptr;
	size_t subscriptionCapacity = 0;
	size_t activeSubscriptionCount = 0;
	SignalWaiterRecord *waiters = nullptr;
	size_t waiterCapacity = 0;
	size_t activeWaiterCount = 0;
	SemaphoreHandle_t queueSpace = nullptr;
	bool initialized = false;
	bool stopping = false;
	TaskHandle_t taskHandle = nullptr;
	bool createdWithCaps = false;
	SignalStackType actualStackType = SignalStackType::Internal;
	uint64_t nextSequence = 1;
	SignalSubscriptionId nextSubscriptionId = 1;
	uint32_t postedCount = 0;
	uint32_t processedEventCount = 0;
	uint32_t callbackInvokeCount = 0;
	uint32_t droppedCount = 0;
	uint32_t rejectedCount = 0;
	uint32_t dispatchErrorCount = 0;
	size_t stackHighWaterMarkBytes = 0;

	void resetCounters() {
		nextSequence = 1;
		nextSubscriptionId = 1;
		postedCount = 0;
		processedEventCount = 0;
		callbackInvokeCount = 0;
		droppedCount = 0;
		rejectedCount = 0;
		dispatchErrorCount = 0;
		stackHighWaterMarkBytes = 0;
	}

	void cleanupStorage() {
		if (waiters != nullptr) {
			for (size_t i = 0; i < waiterCapacity; ++i) {
				if (waiters[i].done != nullptr) {
					vSemaphoreDelete(waiters[i].done);
					waiters[i].done = nullptr;
				}
			}
		}
		if (queueSpace != nullptr) {
			vSemaphoreDelete(queueSpace);
			queueSpace = nullptr;
		}
		delete[] waiters;
		delete[] subscriptions;
		delete[] dispatchMatches;
		delete[] dispatchPayload;
		delete[] queuePayloadStorage;
		delete[] queue;
		waiters = nullptr;
		subscriptions = nullptr;
		dispatchMatches = nullptr;
		dispatchPayload = nullptr;
		queuePayloadStorage = nullptr;
		queue = nullptr;
		subscriptionCapacity = 0;
		activeSubscriptionCount = 0;
		waiterCapacity = 0;
		activeWaiterCount = 0;
		queueHead = 0;
		queueCount = 0;
	}

	void resetRuntimeState() {
		initialized = false;
		stopping = false;
		taskHandle = nullptr;
		createdWithCaps = false;
		actualStackType = SignalStackType::Internal;
		queueHead = 0;
		queueCount = 0;
		activeSubscriptionCount = 0;
		activeWaiterCount = 0;
		resetCounters();
	}

	void cleanupAfterFailedInitLocked() {
		cleanupStorage();
		resetRuntimeState();
	}

	bool allocateStorageLocked(const SignalConfig &newConfig) {
		cleanupStorage();

		queue = new (std::nothrow) SignalQueuedEvent[newConfig.queueSize];
		if (queue == nullptr) {
			cleanupStorage();
			return false;
		}

		const size_t payloadBytes = newConfig.queueSize * newConfig.maxPayloadSize;
		if (payloadBytes > 0) {
			queuePayloadStorage = new (std::nothrow) uint8_t[payloadBytes];
			if (queuePayloadStorage == nullptr) {
				cleanupStorage();
				return false;
			}
		}
		for (size_t i = 0; i < newConfig.queueSize; ++i) {
			queue[i].payload = queuePayloadStorage != nullptr
			                       ? queuePayloadStorage + (i * newConfig.maxPayloadSize)
			                       : nullptr;
		}

		if (newConfig.maxPayloadSize > 0) {
			dispatchPayload = new (std::nothrow) uint8_t[newConfig.maxPayloadSize];
			if (dispatchPayload == nullptr) {
				cleanupStorage();
				return false;
			}
		}

		dispatchMatches = new (std::nothrow) SignalDispatchMatch[newConfig.maxSubscriptions];
		subscriptions = new (std::nothrow) SignalSubscriptionRecord[newConfig.maxSubscriptions];
		if (dispatchMatches == nullptr || subscriptions == nullptr) {
			cleanupStorage();
			return false;
		}
		subscriptionCapacity = newConfig.maxSubscriptions;

		if (newConfig.maxWaiters > 0) {
			waiters = new (std::nothrow) SignalWaiterRecord[newConfig.maxWaiters];
			if (waiters == nullptr) {
				cleanupStorage();
				return false;
			}
			waiterCapacity = newConfig.maxWaiters;
			for (size_t i = 0; i < waiterCapacity; ++i) {
				waiters[i].done = xSemaphoreCreateBinary();
				if (waiters[i].done == nullptr) {
					cleanupStorage();
					return false;
				}
			}
		}

		queueSpace = xSemaphoreCreateCounting(
		    static_cast<UBaseType_t>(newConfig.queueSize),
		    static_cast<UBaseType_t>(newConfig.queueSize)
		);
		if (queueSpace == nullptr) {
			cleanupStorage();
			return false;
		}

		return true;
	}

	void wakeTaskLocked(TaskHandle_t &handle) const {
		handle = taskHandle;
	}

	void enqueueLocked(SignalEventId eventId, size_t payloadSize, const void *payload) {
		const size_t index = (queueHead + queueCount) % config.queueSize;
		SignalQueuedEvent &slot = queue[index];
		slot.eventId = eventId;
		slot.payloadSize = payloadSize;
		slot.sequence = nextSequence++;
		if (payloadSize > 0 && payload != nullptr) {
			memcpy(slot.payload, payload, payloadSize);
		}
		queueCount++;
	}

	bool popLocked(SignalDispatchEvent &event) {
		if (queueCount == 0 || queue == nullptr) {
			return false;
		}

		SignalQueuedEvent &slot = queue[queueHead];
		event.eventId = slot.eventId;
		event.payloadSize = slot.payloadSize;
		event.sequence = slot.sequence;
		event.payload = slot.payloadSize > 0 ? dispatchPayload : nullptr;
		if (slot.payloadSize > 0) {
			memcpy(dispatchPayload, slot.payload, slot.payloadSize);
		}

		queueHead = (queueHead + 1) % config.queueSize;
		queueCount--;
		return true;
	}

	void completeWaitersLocked(
	    SignalEventId eventId,
	    size_t payloadSize,
	    const void *payload,
	    SignalStatus status,
	    const char *message
	) {
		for (size_t i = 0; i < waiterCapacity; ++i) {
			SignalWaiterRecord &waiter = waiters[i];
			if (!waiter.inUse || waiter.completed || waiter.eventId != eventId ||
			    waiter.payloadSize != payloadSize) {
				continue;
			}
			if (payloadSize > 0 && waiter.payloadOut != nullptr && payload != nullptr) {
				memcpy(waiter.payloadOut, payload, payloadSize);
			}
			waiter.status = status;
			waiter.message = message != nullptr ? message : "signal wait completed";
			waiter.completed = true;
			xSemaphoreGive(waiter.done);
		}
	}

	void failAllWaitersLocked(SignalStatus status, const char *message) {
		for (size_t i = 0; i < waiterCapacity; ++i) {
			SignalWaiterRecord &waiter = waiters[i];
			if (!waiter.inUse || waiter.completed) {
				continue;
			}
			waiter.status = status;
			waiter.message = message != nullptr ? message : "signal stopped";
			waiter.completed = true;
			xSemaphoreGive(waiter.done);
		}
	}

	SignalWaiterRecord *findFreeWaiterLocked() {
		for (size_t i = 0; i < waiterCapacity; ++i) {
			if (!waiters[i].inUse) {
				return &waiters[i];
			}
		}
		return nullptr;
	}

	void releaseWaiterLocked(SignalWaiterRecord *target) {
		if (target == nullptr || !target->inUse) {
			return;
		}
		target->eventId = 0;
		target->payloadSize = 0;
		target->payloadOut = nullptr;
		target->inUse = false;
		target->completed = false;
		target->status = SignalStatus::Timeout;
		target->message = "signal wait timed out";
		if (activeWaiterCount > 0) {
			activeWaiterCount--;
		}
	}

	SignalSubscriptionRecord *findFreeSubscriptionLocked() {
		for (size_t i = 0; i < subscriptionCapacity; ++i) {
			if (subscriptions[i].available()) {
				return &subscriptions[i];
			}
		}
		return nullptr;
	}

	void cleanupSubscriptionLocked(SignalSubscriptionRecord &slot) {
		if (slot.active || slot.dispatchRefs > 0) {
			return;
		}
		slot.clearCallback();
		slot.pendingCleanup = false;
	}

	void finishSubscriptionDispatchLocked(size_t index) {
		if (index >= subscriptionCapacity) {
			return;
		}
		SignalSubscriptionRecord &slot = subscriptions[index];
		if (slot.dispatchRefs > 0) {
			slot.dispatchRefs--;
		}
		if (!slot.active && slot.dispatchRefs == 0 && slot.pendingCleanup) {
			cleanupSubscriptionLocked(slot);
		}
	}

	size_t collectMatchesLocked(const SignalDispatchEvent &event) {
		size_t count = 0;
		for (size_t i = 0; i < subscriptionCapacity && count < subscriptionCapacity; ++i) {
			SignalSubscriptionRecord &subscription = subscriptions[i];
			if (!subscription.active || subscription.eventId != event.eventId ||
			    subscription.payloadSize != event.payloadSize ||
			    subscription.kind == SignalCallbackKind::None) {
				continue;
			}
			if (subscription.dispatchRefs == UINT16_MAX) {
				dispatchErrorCount++;
				continue;
			}
			subscription.dispatchRefs++;
			dispatchMatches[count].index = i;
			dispatchMatches[count].id = subscription.id;
			dispatchMatches[count].generation = subscription.generation;
			count++;
		}
		return count;
	}

	bool popNext(SignalDispatchEvent &event) {
		SignalLock lock(mutex);
		if (!lock || !popLocked(event)) {
			return false;
		}
		if (queueSpace != nullptr) {
			xSemaphoreGive(queueSpace);
		}
		return true;
	}

	bool isStoppingAndEmpty() {
		SignalLock lock(mutex);
		return lock && stopping && queueCount == 0;
	}

	void markTaskStopped() {
		SignalLock lock(mutex);
		if (lock) {
			stackHighWaterMarkBytes = signal_task_support::currentStackHighWaterMarkBytes();
			taskHandle = nullptr;
		}
	}

	static void taskEntry(void *arg) {
		SignalImpl *impl = static_cast<SignalImpl *>(arg);
		if (impl == nullptr) {
			vTaskDelete(nullptr);
			return;
		}

		while (true) {
			SignalDispatchEvent event;
			if (impl->popNext(event)) {
				size_t matchCount = 0;
				{
					SignalLock lock(impl->mutex);
					if (lock) {
						matchCount = impl->collectMatchesLocked(event);
					}
				}

				for (size_t i = 0; i < matchCount; ++i) {
					const SignalDispatchMatch match = impl->dispatchMatches[i];
					bool invoked = false;
					SignalCallbackKind kind = SignalCallbackKind::None;
					SignalRawCallback rawCallback = nullptr;
					SignalRawPayloadCallback rawPayloadCallback = nullptr;
					void *context = nullptr;

					{
						SignalLock lock(impl->mutex);
						if (!lock || match.index >= impl->subscriptionCapacity) {
							if (lock) {
								impl->dispatchErrorCount++;
							}
							continue;
						}
						SignalSubscriptionRecord &slot = impl->subscriptions[match.index];
						if (!slot.active || slot.id != match.id ||
						    slot.generation != match.generation) {
							impl->finishSubscriptionDispatchLocked(match.index);
							continue;
						}
						kind = slot.kind;
						rawCallback = slot.rawCallback;
						rawPayloadCallback = slot.rawPayloadCallback;
						context = slot.context;
					}

					if (kind == SignalCallbackKind::Raw && rawCallback != nullptr) {
						rawCallback(context);
						invoked = true;
					} else if (
					    kind == SignalCallbackKind::RawPayload &&
					    rawPayloadCallback != nullptr
					) {
						rawPayloadCallback(context, event.payload, event.payloadSize);
						invoked = true;
					} else if (kind == SignalCallbackKind::Function) {
						SignalSubscriptionRecord *slot = nullptr;
						{
							SignalLock lock(impl->mutex);
							if (lock && match.index < impl->subscriptionCapacity) {
								SignalSubscriptionRecord &candidate =
								    impl->subscriptions[match.index];
								if (candidate.active && candidate.id == match.id &&
								    candidate.generation == match.generation &&
								    candidate.functionCallback) {
									slot = &candidate;
								}
							}
						}
						if (slot != nullptr) {
							slot->functionCallback(event.payload, event.payloadSize);
							invoked = true;
						}
					}

					{
						SignalLock lock(impl->mutex);
						if (lock) {
							if (invoked) {
								impl->callbackInvokeCount++;
							} else {
								impl->dispatchErrorCount++;
							}
							impl->finishSubscriptionDispatchLocked(match.index);
						}
					}
				}

				{
					SignalLock lock(impl->mutex);
					if (lock) {
						impl->processedEventCount++;
					}
				}
				continue;
			}

			if (impl->isStoppingAndEmpty()) {
				break;
			}

			ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		}

		const bool withCaps = impl->createdWithCaps;
		impl->markTaskStopped();
		signal_task_support::deleteCurrentTask(withCaps);
	}
};

SignalResult SignalResult::success(const char *message) {
	SignalResult result;
	result.result = true;
	result.status = SignalStatus::Ok;
	result.message = message != nullptr ? message : "ok";
	return result;
}

SignalResult SignalResult::failure(SignalStatus status, const char *message) {
	SignalResult result;
	result.result = false;
	result.status = status;
	result.message = message != nullptr ? message : "error";
	return result;
}

SignalSubResult SignalSubResult::success(SignalSubscriptionId id, const char *message) {
	SignalSubResult result;
	result.result = true;
	result.status = SignalStatus::Ok;
	result.message = message != nullptr ? message : "ok";
	result.id = id;
	return result;
}

SignalSubResult SignalSubResult::failure(
    SignalStatus status,
    const char *message,
    SignalSubscriptionId id
) {
	SignalSubResult result;
	result.result = false;
	result.status = status;
	result.message = message != nullptr ? message : "error";
	result.id = id;
	return result;
}

SignalSubscriptionHandle::SignalSubscriptionHandle(Signal *bus, SignalSubscriptionId id)
    : _bus(bus), _id(id) {
}

SignalSubscriptionHandle::~SignalSubscriptionHandle() {
	unsubscribe();
}

SignalSubscriptionHandle::SignalSubscriptionHandle(SignalSubscriptionHandle &&other) noexcept
    : _bus(other._bus), _id(other._id) {
	other._bus = nullptr;
	other._id = 0;
}

SignalSubscriptionHandle &SignalSubscriptionHandle::operator=(
    SignalSubscriptionHandle &&other
) noexcept {
	if (this != &other) {
		unsubscribe();
		_bus = other._bus;
		_id = other._id;
		other._bus = nullptr;
		other._id = 0;
	}
	return *this;
}

SignalResult SignalSubscriptionHandle::unsubscribe() {
	if (_bus == nullptr || _id == kInvalidSubscriptionId) {
		return SignalResult::success("signal subscription handle is empty");
	}
	Signal *bus = _bus;
	const SignalSubscriptionId id = _id;
	_bus = nullptr;
	_id = 0;
	return bus->unsubscribe(id);
}

SignalSubscriptionId SignalSubscriptionHandle::release() {
	const SignalSubscriptionId id = _id;
	_bus = nullptr;
	_id = 0;
	return id;
}

Signal::Signal() : _impl(new (std::nothrow) SignalImpl()) {
}

Signal::~Signal() {
	if (_impl != nullptr) {
		end(2000);
	}
}

SignalResult Signal::init(const SignalConfig &config) {
	if (_impl == nullptr) {
		return allocationFailure();
	}
	if (!signal_task_support::isValidStackSize(config.stackSizeBytes)) {
		return SignalResult::failure(
		    SignalStatus::InvalidArgument,
		    "stack size must be at least 1024 bytes and aligned"
		);
	}
	if (config.taskName == nullptr || config.taskName[0] == '\0') {
		return SignalResult::failure(SignalStatus::InvalidArgument, "task name is required");
	}
	if (config.queueSize == 0) {
		return SignalResult::failure(SignalStatus::InvalidArgument, "queue size must be greater than zero");
	}
	if (config.maxSubscriptions == 0) {
		return SignalResult::failure(
		    SignalStatus::InvalidArgument,
		    "max subscriptions must be greater than zero"
		);
	}

	bool usePsramStack = false;
	SignalStackType actualStackType = SignalStackType::Internal;
	if (config.stackType == SignalStackType::Psram) {
		if (!signal_task_support::hasExternalStackSupport()) {
			return SignalResult::failure(
			    SignalStatus::TaskCreateFailed,
			    "PSRAM task stacks are not available"
			);
		}
		usePsramStack = true;
		actualStackType = SignalStackType::Psram;
	} else if (
	    config.stackType == SignalStackType::Auto &&
	    signal_task_support::hasExternalStackSupport()
	) {
		usePsramStack = true;
		actualStackType = SignalStackType::Psram;
	}

	{
		SignalLock lock(_impl->mutex);
		if (!lock) {
			return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
		}
		if (_impl->initialized) {
			return SignalResult::failure(SignalStatus::AlreadyInitialized, "signal already initialized");
		}
		_impl->cleanupAfterFailedInitLocked();
		if (!_impl->allocateStorageLocked(config)) {
			_impl->cleanupAfterFailedInitLocked();
			return SignalResult::failure(SignalStatus::OutOfMemory, "failed to allocate signal storage");
		}

		_impl->config = config;
		_impl->actualStackType = actualStackType;
		_impl->stopping = false;
		_impl->createdWithCaps = false;
		_impl->resetCounters();
	}

	TaskHandle_t handle = nullptr;
	bool createdWithCaps = false;
	const BaseType_t created = signal_task_support::createTask(
	    &SignalImpl::taskEntry,
	    config.taskName,
	    config.stackSizeBytes,
	    _impl.get(),
	    config.priority,
	    &handle,
	    config.coreId,
	    usePsramStack,
	    createdWithCaps
	);
	if (created != pdPASS || handle == nullptr) {
		SignalLock lock(_impl->mutex);
		if (lock) {
			_impl->cleanupAfterFailedInitLocked();
		}
		return SignalResult::failure(SignalStatus::TaskCreateFailed, "failed to create signal task");
	}

	{
		SignalLock lock(_impl->mutex);
		if (lock) {
			_impl->taskHandle = handle;
			_impl->createdWithCaps = createdWithCaps;
			_impl->initialized = true;
		}
	}

	return SignalResult::success("signal initialized");
}

SignalResult Signal::end(uint32_t timeoutMs) {
	if (_impl == nullptr) {
		return allocationFailure();
	}

	TaskHandle_t handle = nullptr;
	{
		SignalLock lock(_impl->mutex);
		if (!lock) {
			return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
		}
		if (!_impl->initialized) {
			return SignalResult::success("signal is not initialized");
		}
		if (_impl->taskHandle != nullptr && xTaskGetCurrentTaskHandle() == _impl->taskHandle) {
			return SignalResult::failure(
			    SignalStatus::InvalidArgument,
			    "end cannot be called from the signal task"
			);
		}
		_impl->stopping = true;
		_impl->failAllWaitersLocked(SignalStatus::NotInitialized, "signal is ending");
		_impl->wakeTaskLocked(handle);
	}
	if (handle != nullptr) {
		xTaskNotifyGive(handle);
	}

	const uint32_t startedMs = millis();
	while (true) {
		{
			SignalLock lock(_impl->mutex);
			if (lock && _impl->taskHandle == nullptr) {
				_impl->cleanupStorage();
				_impl->initialized = false;
				_impl->stopping = false;
				_impl->queueHead = 0;
				_impl->queueCount = 0;
				return SignalResult::success("signal ended");
			}
		}
		if (timeoutElapsed(startedMs, timeoutMs)) {
			return SignalResult::failure(SignalStatus::Timeout, "signal end timed out");
		}
		vTaskDelay(pdMS_TO_TICKS(1));
	}
}

SignalSubResult Signal::subscribe(SignalEventId eventId, SignalCallback callback) {
	if (!callback) {
		return SignalSubResult::failure(SignalStatus::InvalidArgument, "callback is required");
	}
	return subscribeFunction(
	    eventId,
	    0,
	    [callback](const void *, size_t) {
		    callback();
	    }
	);
}

SignalSubscriptionHandle Signal::subscribeHandle(
    SignalEventId eventId,
    SignalCallback callback
) {
	SignalSubResult result = subscribe(eventId, callback);
	if (!result) {
		return SignalSubscriptionHandle();
	}
	return SignalSubscriptionHandle(this, result.id);
}

SignalSubResult Signal::subscribeRaw(
    SignalEventId eventId,
    SignalRawCallback callback,
    void *context
) {
	if (_impl == nullptr) {
		return subscriptionAllocationFailure();
	}
	if (callback == nullptr) {
		return SignalSubResult::failure(SignalStatus::InvalidArgument, "callback is required");
	}

	SignalLock lock(_impl->mutex);
	if (!lock) {
		return SignalSubResult::failure(SignalStatus::InternalError, "failed to lock signal");
	}
	if (!_impl->initialized || _impl->stopping) {
		_impl->rejectedCount++;
		return SignalSubResult::failure(SignalStatus::NotInitialized, "signal is not initialized");
	}

	SignalSubscriptionRecord *slot = _impl->findFreeSubscriptionLocked();
	if (slot == nullptr) {
		_impl->rejectedCount++;
		return SignalSubResult::failure(
		    SignalStatus::TooManySubscriptions,
		    "maximum subscriptions reached"
		);
	}

	const SignalSubscriptionId id = _impl->nextSubscriptionId++;
	slot->id = id;
	slot->generation++;
	slot->active = true;
	slot->pendingCleanup = false;
	slot->eventId = eventId;
	slot->payloadSize = 0;
	slot->kind = SignalCallbackKind::Raw;
	slot->rawCallback = callback;
	slot->rawPayloadCallback = nullptr;
	slot->context = context;
	_impl->activeSubscriptionCount++;
	return SignalSubResult::success(id, "signal subscription added");
}

SignalSubResult Signal::subscribeRaw(
    SignalEventId eventId,
    size_t payloadSize,
    SignalRawPayloadCallback callback,
    void *context
) {
	if (_impl == nullptr) {
		return subscriptionAllocationFailure();
	}
	if (callback == nullptr) {
		return SignalSubResult::failure(SignalStatus::InvalidArgument, "callback is required");
	}

	SignalLock lock(_impl->mutex);
	if (!lock) {
		return SignalSubResult::failure(SignalStatus::InternalError, "failed to lock signal");
	}
	if (!_impl->initialized || _impl->stopping) {
		_impl->rejectedCount++;
		return SignalSubResult::failure(SignalStatus::NotInitialized, "signal is not initialized");
	}
	if (payloadSize > _impl->config.maxPayloadSize) {
		_impl->rejectedCount++;
		return SignalSubResult::failure(SignalStatus::InvalidArgument, "payload is too large");
	}

	SignalSubscriptionRecord *slot = _impl->findFreeSubscriptionLocked();
	if (slot == nullptr) {
		_impl->rejectedCount++;
		return SignalSubResult::failure(
		    SignalStatus::TooManySubscriptions,
		    "maximum subscriptions reached"
		);
	}

	const SignalSubscriptionId id = _impl->nextSubscriptionId++;
	slot->id = id;
	slot->generation++;
	slot->active = true;
	slot->pendingCleanup = false;
	slot->eventId = eventId;
	slot->payloadSize = payloadSize;
	slot->kind = SignalCallbackKind::RawPayload;
	slot->rawCallback = nullptr;
	slot->rawPayloadCallback = callback;
	slot->context = context;
	_impl->activeSubscriptionCount++;
	return SignalSubResult::success(id, "signal subscription added");
}

SignalSubscriptionHandle Signal::subscribeRawHandle(
    SignalEventId eventId,
    SignalRawCallback callback,
    void *context
) {
	SignalSubResult result = subscribeRaw(eventId, callback, context);
	if (!result) {
		return SignalSubscriptionHandle();
	}
	return SignalSubscriptionHandle(this, result.id);
}

SignalSubscriptionHandle Signal::subscribeRawHandle(
    SignalEventId eventId,
    size_t payloadSize,
    SignalRawPayloadCallback callback,
    void *context
) {
	SignalSubResult result = subscribeRaw(eventId, payloadSize, callback, context);
	if (!result) {
		return SignalSubscriptionHandle();
	}
	return SignalSubscriptionHandle(this, result.id);
}

SignalSubResult Signal::subscribeFunction(
    SignalEventId eventId,
    size_t payloadSize,
    SignalFunctionCallback callback
) {
	if (_impl == nullptr) {
		return subscriptionAllocationFailure();
	}
	if (!callback) {
		return SignalSubResult::failure(SignalStatus::InvalidArgument, "callback is required");
	}

	SignalLock lock(_impl->mutex);
	if (!lock) {
		return SignalSubResult::failure(SignalStatus::InternalError, "failed to lock signal");
	}
	if (!_impl->initialized || _impl->stopping) {
		_impl->rejectedCount++;
		return SignalSubResult::failure(SignalStatus::NotInitialized, "signal is not initialized");
	}
	if (payloadSize > _impl->config.maxPayloadSize) {
		_impl->rejectedCount++;
		return SignalSubResult::failure(SignalStatus::InvalidArgument, "payload is too large");
	}

	SignalSubscriptionRecord *slot = _impl->findFreeSubscriptionLocked();
	if (slot == nullptr) {
		_impl->rejectedCount++;
		return SignalSubResult::failure(
		    SignalStatus::TooManySubscriptions,
		    "maximum subscriptions reached"
		);
	}

	const SignalSubscriptionId id = _impl->nextSubscriptionId++;
	slot->id = id;
	slot->generation++;
	slot->active = true;
	slot->pendingCleanup = false;
	slot->eventId = eventId;
	slot->payloadSize = payloadSize;
	slot->kind = SignalCallbackKind::Function;
	slot->rawCallback = nullptr;
	slot->rawPayloadCallback = nullptr;
	slot->context = nullptr;
	slot->functionCallback = callback;
	_impl->activeSubscriptionCount++;
	return SignalSubResult::success(id, "signal subscription added");
}

SignalResult Signal::unsubscribe(SignalSubscriptionId id) {
	if (_impl == nullptr) {
		return allocationFailure();
	}
	if (id == kInvalidSubscriptionId) {
		return SignalResult::failure(SignalStatus::InvalidArgument, "subscription id is required");
	}

	SignalLock lock(_impl->mutex);
	if (!lock) {
		return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
	}
	if (!_impl->initialized || _impl->stopping) {
		_impl->rejectedCount++;
		return SignalResult::failure(SignalStatus::NotInitialized, "signal is not initialized");
	}

	for (size_t i = 0; i < _impl->subscriptionCapacity; ++i) {
		SignalSubscriptionRecord &slot = _impl->subscriptions[i];
		if (!slot.active || slot.id != id) {
			continue;
		}
		slot.active = false;
		slot.pendingCleanup = true;
		slot.generation++;
		if (_impl->activeSubscriptionCount > 0) {
			_impl->activeSubscriptionCount--;
		}
		if (slot.dispatchRefs == 0) {
			_impl->cleanupSubscriptionLocked(slot);
		}
		return SignalResult::success("signal subscription removed");
	}

	_impl->rejectedCount++;
	return SignalResult::failure(SignalStatus::SubscriptionNotFound, "subscription not found");
}

SignalResult Signal::post(SignalEventId eventId) {
	return postRaw(eventId, 0, nullptr, 0, true);
}

SignalResult Signal::postWithTimeout(SignalEventId eventId, uint32_t timeoutMs) {
	return postRaw(eventId, 0, nullptr, timeoutMs, false);
}

SignalResult Signal::postRaw(
    SignalEventId eventId,
    size_t payloadSize,
    const void *payload,
    uint32_t timeoutMs,
    bool useDefaultTimeout
) {
	if (_impl == nullptr) {
		return allocationFailure();
	}
	if (payloadSize > 0 && payload == nullptr) {
		return SignalResult::failure(SignalStatus::InvalidArgument, "payload is required");
	}

	const uint32_t startedMs = millis();
	uint32_t effectiveTimeout = timeoutMs;
	{
		SignalLock lock(_impl->mutex);
		if (!lock) {
			return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
		}
		effectiveTimeout = useDefaultTimeout ? _impl->config.defaultPostTimeoutMs : timeoutMs;
	}

	while (true) {
		TaskHandle_t handle = nullptr;
		bool shouldNotify = false;

		{
			SignalLock lock(_impl->mutex);
			if (!lock) {
				return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
			}
			if (!_impl->initialized || _impl->stopping) {
				_impl->rejectedCount++;
				return SignalResult::failure(SignalStatus::NotInitialized, "signal is not initialized");
			}
			if (payloadSize > _impl->config.maxPayloadSize) {
				_impl->rejectedCount++;
				return SignalResult::failure(SignalStatus::InvalidArgument, "payload is too large");
			}

			if (_impl->queueCount < _impl->config.queueSize) {
				if (_impl->queueSpace != nullptr &&
				    xSemaphoreTake(_impl->queueSpace, 0) != pdTRUE) {
					_impl->dispatchErrorCount++;
					return SignalResult::failure(
					    SignalStatus::InternalError,
					    "signal queue space is inconsistent"
					);
				}
				_impl->enqueueLocked(eventId, payloadSize, payload);
				_impl->completeWaitersLocked(
				    eventId,
				    payloadSize,
				    payload,
				    SignalStatus::Ok,
				    "signal event received"
				);
				_impl->postedCount++;
				_impl->wakeTaskLocked(handle);
				shouldNotify = true;
			} else if (_impl->config.overflowPolicy == SignalOverflowPolicy::DropNewest) {
				_impl->droppedCount++;
				_impl->rejectedCount++;
				return SignalResult::failure(SignalStatus::QueueFull, "signal queue is full");
			} else if (_impl->config.overflowPolicy == SignalOverflowPolicy::DropOldest) {
				_impl->queueHead = (_impl->queueHead + 1) % _impl->config.queueSize;
				_impl->queueCount--;
				_impl->droppedCount++;
				_impl->enqueueLocked(eventId, payloadSize, payload);
				_impl->completeWaitersLocked(
				    eventId,
				    payloadSize,
				    payload,
				    SignalStatus::Ok,
				    "signal event received"
				);
				_impl->postedCount++;
				_impl->wakeTaskLocked(handle);
				shouldNotify = true;
			} else if (effectiveTimeout == 0) {
				_impl->droppedCount++;
				_impl->rejectedCount++;
				return SignalResult::failure(SignalStatus::Timeout, "signal queue is full");
			}
		}

		if (shouldNotify) {
			if (handle != nullptr) {
				xTaskNotifyGive(handle);
			}
			return SignalResult::success("signal event queued");
		}

		if (timeoutElapsed(startedMs, effectiveTimeout)) {
			SignalLock lock(_impl->mutex);
			if (lock) {
				_impl->droppedCount++;
				_impl->rejectedCount++;
			}
			return SignalResult::failure(SignalStatus::Timeout, "signal queue is full");
		}
		const uint32_t elapsed = millis() - startedMs;
		const uint32_t remaining =
		    effectiveTimeout == portMAX_DELAY ? portMAX_DELAY : effectiveTimeout - elapsed;
		if (_impl->queueSpace == nullptr ||
		    xSemaphoreTake(_impl->queueSpace, timeoutToTicks(remaining)) != pdTRUE) {
			SignalLock lock(_impl->mutex);
			if (lock) {
				_impl->droppedCount++;
				_impl->rejectedCount++;
			}
			return SignalResult::failure(SignalStatus::Timeout, "signal queue is full");
		}

		{
			SignalLock lock(_impl->mutex);
			if (!lock) {
				xSemaphoreGive(_impl->queueSpace);
				return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
			}
			if (!_impl->initialized || _impl->stopping) {
				xSemaphoreGive(_impl->queueSpace);
				_impl->rejectedCount++;
				return SignalResult::failure(SignalStatus::NotInitialized, "signal is not initialized");
			}
			if (payloadSize > _impl->config.maxPayloadSize || _impl->queueCount >= _impl->config.queueSize) {
				xSemaphoreGive(_impl->queueSpace);
				if (timeoutElapsed(startedMs, effectiveTimeout)) {
					_impl->droppedCount++;
					_impl->rejectedCount++;
					return SignalResult::failure(SignalStatus::Timeout, "signal queue is full");
				}
				continue;
			}
			_impl->enqueueLocked(eventId, payloadSize, payload);
			_impl->completeWaitersLocked(
			    eventId,
			    payloadSize,
			    payload,
			    SignalStatus::Ok,
			    "signal event received"
			);
			_impl->postedCount++;
			_impl->wakeTaskLocked(handle);
		}
		if (handle != nullptr) {
			xTaskNotifyGive(handle);
		}
		return SignalResult::success("signal event queued");
	}
}

SignalResult Signal::waitFor(SignalEventId eventId, uint32_t timeoutMs) {
	return waitForRaw(eventId, 0, nullptr, timeoutMs);
}

SignalResult Signal::waitForRaw(
    SignalEventId eventId,
    size_t payloadSize,
    void *payloadOut,
    uint32_t timeoutMs
) {
	if (_impl == nullptr) {
		return allocationFailure();
	}
	if (payloadSize > 0 && payloadOut == nullptr) {
		return SignalResult::failure(SignalStatus::InvalidArgument, "payload output is required");
	}

	SignalWaiterRecord *waiter = nullptr;
	SemaphoreHandle_t done = nullptr;
	{
		SignalLock lock(_impl->mutex);
		if (!lock) {
			return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
		}
		if (!_impl->initialized || _impl->stopping) {
			_impl->rejectedCount++;
			return SignalResult::failure(SignalStatus::NotInitialized, "signal is not initialized");
		}
		if (_impl->taskHandle != nullptr && xTaskGetCurrentTaskHandle() == _impl->taskHandle) {
			_impl->rejectedCount++;
			return SignalResult::failure(
			    SignalStatus::InvalidArgument,
			    "waitFor cannot be called from the signal task"
			);
		}
		if (payloadSize > _impl->config.maxPayloadSize) {
			_impl->rejectedCount++;
			return SignalResult::failure(SignalStatus::InvalidArgument, "payload is too large");
		}
		waiter = _impl->findFreeWaiterLocked();
		if (waiter == nullptr) {
			_impl->rejectedCount++;
			return SignalResult::failure(SignalStatus::TooManyWaiters, "maximum waiters reached");
		}
		xSemaphoreTake(waiter->done, 0);
		waiter->eventId = eventId;
		waiter->payloadSize = payloadSize;
		waiter->payloadOut = payloadOut;
		waiter->inUse = true;
		waiter->completed = false;
		waiter->status = SignalStatus::Timeout;
		waiter->message = "signal wait timed out";
		_impl->activeWaiterCount++;
		done = waiter->done;
	}

	xSemaphoreTake(done, timeoutToTicks(timeoutMs));

	SignalResult result;
	{
		SignalLock lock(_impl->mutex);
		if (!lock) {
			return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
		}
		if (waiter->completed) {
			result = waiter->status == SignalStatus::Ok
			             ? SignalResult::success(waiter->message)
			             : SignalResult::failure(waiter->status, waiter->message);
		} else {
			result = SignalResult::failure(SignalStatus::Timeout, "signal wait timed out");
		}
		_impl->releaseWaiterLocked(waiter);
	}

	return result;
}

SignalDiag Signal::getDiagnostics() {
	SignalDiag diag;
	if (_impl == nullptr) {
		return diag;
	}
	SignalLock lock(_impl->mutex);
	if (!lock) {
		return diag;
	}
	diag.postedCount = _impl->postedCount;
	diag.processedEventCount = _impl->processedEventCount;
	diag.callbackInvokeCount = _impl->callbackInvokeCount;
	diag.dispatchedCount = _impl->processedEventCount;
	diag.droppedCount = _impl->droppedCount;
	diag.rejectedCount = _impl->rejectedCount;
	diag.queueSize = _impl->config.queueSize;
	diag.queueUsed = _impl->queueCount;
	diag.subscriptionCount = _impl->activeSubscriptionCount;
	diag.waiterCount = _impl->activeWaiterCount;
	diag.dispatchErrorCount = _impl->dispatchErrorCount;
	diag.stackHighWaterMarkBytes = _impl->stackHighWaterMarkBytes;
	diag.requestedStackType = _impl->config.stackType;
	diag.actualStackType = _impl->actualStackType;
	return diag;
}

const char *Signal::statusToString(SignalStatus status) const {
	switch (status) {
	case SignalStatus::Ok:
		return "Ok";
	case SignalStatus::NotInitialized:
		return "NotInitialized";
	case SignalStatus::AlreadyInitialized:
		return "AlreadyInitialized";
	case SignalStatus::InvalidArgument:
		return "InvalidArgument";
	case SignalStatus::OutOfMemory:
		return "OutOfMemory";
	case SignalStatus::TaskCreateFailed:
		return "TaskCreateFailed";
	case SignalStatus::SubscriptionNotFound:
		return "SubscriptionNotFound";
	case SignalStatus::QueueFull:
		return "QueueFull";
	case SignalStatus::TooManySubscriptions:
		return "TooManySubscriptions";
	case SignalStatus::TooManyWaiters:
		return "TooManyWaiters";
	case SignalStatus::Busy:
		return "Busy";
	case SignalStatus::Timeout:
		return "Timeout";
	case SignalStatus::InternalError:
		return "InternalError";
	default:
		return "Unknown";
	}
}

} // namespace zek::signal
