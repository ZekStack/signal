#include "Signal.h"

#include "internal/SignalMutex.h"
#include "internal/SignalTaskSupport.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include <freertos/semphr.h>

namespace {
constexpr SignalSubscriptionId kInvalidSubscriptionId = 0;
constexpr uint32_t kWaitPollMs = 10;
using SignalRawCallbackImpl = std::function<void(const void *, size_t)>;

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
} // namespace

struct SignalQueuedEvent {
	SignalEventId eventId = 0;
	size_t payloadSize = 0;
	uint64_t sequence = 0;
	std::vector<uint8_t> payload;
};

struct SignalDispatchEvent {
	SignalEventId eventId = 0;
	size_t payloadSize = 0;
	uint64_t sequence = 0;
	std::vector<uint8_t> payload;
};

struct SignalSubscriptionRecord {
	SignalSubscriptionId id = kInvalidSubscriptionId;
	SignalEventId eventId = 0;
	size_t payloadSize = 0;
	SignalRawCallbackImpl callback;
};

struct SignalWaiterRecord {
	SignalEventId eventId = 0;
	size_t payloadSize = 0;
	void *payloadOut = nullptr;
	SemaphoreHandle_t done = nullptr;
	bool completed = false;
	SignalStatus status = SignalStatus::Timeout;
	const char *message = "signal wait timed out";
};

struct SignalImpl {
	SignalConfig config{};
	SignalMutex mutex;
	std::vector<SignalQueuedEvent> queue;
	size_t queueHead = 0;
	size_t queueCount = 0;
	std::vector<SignalSubscriptionRecord> subscriptions;
	std::vector<SignalWaiterRecord *> waiters;
	bool initialized = false;
	bool stopping = false;
	TaskHandle_t taskHandle = nullptr;
	bool createdWithCaps = false;
	SignalStackType actualStackType = SignalStackType::Internal;
	uint64_t nextSequence = 1;
	SignalSubscriptionId nextSubscriptionId = 1;
	uint32_t postedCount = 0;
	uint32_t dispatchedCount = 0;
	uint32_t droppedCount = 0;
	uint32_t dispatchErrorCount = 0;
	size_t stackHighWaterMarkBytes = 0;

	void wakeTask() {
		TaskHandle_t handle = nullptr;
		{
			SignalLock lock(mutex);
			if (lock) {
				handle = taskHandle;
			}
		}
		if (handle != nullptr) {
			xTaskNotifyGive(handle);
		}
	}

	void enqueueLocked(SignalEventId eventId, size_t payloadSize, const void *payload) {
		const size_t index = (queueHead + queueCount) % queue.size();
		SignalQueuedEvent &slot = queue[index];
		slot.eventId = eventId;
		slot.payloadSize = payloadSize;
		slot.sequence = nextSequence++;
		if (payloadSize > 0 && payload != nullptr) {
			memcpy(slot.payload.data(), payload, payloadSize);
		}
		queueCount++;
	}

	bool popLocked(SignalDispatchEvent &event) {
		if (queueCount == 0 || queue.empty()) {
			return false;
		}

		SignalQueuedEvent &slot = queue[queueHead];
		event.eventId = slot.eventId;
		event.payloadSize = slot.payloadSize;
		event.sequence = slot.sequence;
		event.payload.resize(slot.payloadSize);
		if (slot.payloadSize > 0) {
			memcpy(event.payload.data(), slot.payload.data(), slot.payloadSize);
		}

		queueHead = (queueHead + 1) % queue.size();
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
		for (SignalWaiterRecord *waiter : waiters) {
			if (waiter == nullptr || waiter->completed || waiter->eventId != eventId ||
			    waiter->payloadSize != payloadSize) {
				continue;
			}
			if (payloadSize > 0 && waiter->payloadOut != nullptr && payload != nullptr) {
				memcpy(waiter->payloadOut, payload, payloadSize);
			}
			waiter->status = status;
			waiter->message = message != nullptr ? message : "signal wait completed";
			waiter->completed = true;
			if (waiter->done != nullptr) {
				xSemaphoreGive(waiter->done);
			}
		}
	}

	void failAllWaitersLocked(SignalStatus status, const char *message) {
		for (SignalWaiterRecord *waiter : waiters) {
			if (waiter == nullptr || waiter->completed) {
				continue;
			}
			waiter->status = status;
			waiter->message = message != nullptr ? message : "signal stopped";
			waiter->completed = true;
			if (waiter->done != nullptr) {
				xSemaphoreGive(waiter->done);
			}
		}
	}

	void removeWaiterLocked(SignalWaiterRecord *target) {
		waiters.erase(
		    std::remove(waiters.begin(), waiters.end(), target),
		    waiters.end()
		);
	}

	std::vector<SignalRawCallbackImpl> matchingCallbacks(const SignalDispatchEvent &event) {
		std::vector<SignalRawCallbackImpl> callbacks;
		SignalLock lock(mutex);
		if (!lock) {
			return callbacks;
		}
		for (const SignalSubscriptionRecord &subscription : subscriptions) {
			if (subscription.eventId == event.eventId &&
			    subscription.payloadSize == event.payloadSize && subscription.callback) {
				callbacks.push_back(subscription.callback);
			}
		}
		return callbacks;
	}

	bool popNext(SignalDispatchEvent &event) {
		SignalLock lock(mutex);
		return lock && popLocked(event);
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
				std::vector<SignalRawCallbackImpl> callbacks = impl->matchingCallbacks(event);
				for (const SignalRawCallbackImpl &callback : callbacks) {
					if (!callback) {
						SignalLock lock(impl->mutex);
						if (lock) {
							impl->dispatchErrorCount++;
						}
						continue;
					}
					callback(
					    event.payloadSize > 0 ? event.payload.data() : nullptr,
					    event.payloadSize
					);
				}
				{
					SignalLock lock(impl->mutex);
					if (lock) {
						impl->dispatchedCount++;
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

Signal::Signal() : _impl(std::make_unique<SignalImpl>()) {
}

Signal::~Signal() {
	end(2000);
}

SignalResult Signal::init(const SignalConfig &config) {
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

		_impl->config = config;
		_impl->actualStackType = actualStackType;
		_impl->queue.clear();
		_impl->queue.resize(config.queueSize);
		for (SignalQueuedEvent &slot : _impl->queue) {
			slot.payload.resize(config.maxPayloadSize);
		}
		_impl->subscriptions.clear();
		_impl->subscriptions.reserve(config.maxSubscriptions);
		_impl->waiters.clear();
		_impl->waiters.reserve(config.maxWaiters);
		_impl->queueHead = 0;
		_impl->queueCount = 0;
		_impl->stopping = false;
		_impl->createdWithCaps = false;
		_impl->nextSequence = 1;
		_impl->nextSubscriptionId = 1;
		_impl->postedCount = 0;
		_impl->dispatchedCount = 0;
		_impl->droppedCount = 0;
		_impl->dispatchErrorCount = 0;
		_impl->stackHighWaterMarkBytes = 0;
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
			_impl->queue.clear();
			_impl->subscriptions.clear();
			_impl->waiters.clear();
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
	TaskHandle_t handle = nullptr;
	{
		SignalLock lock(_impl->mutex);
		if (!lock) {
			return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
		}
		if (!_impl->initialized) {
			return SignalResult::success("signal is not initialized");
		}
		_impl->stopping = true;
		_impl->failAllWaitersLocked(SignalStatus::NotInitialized, "signal is ending");
		handle = _impl->taskHandle;
	}
	if (handle != nullptr) {
		xTaskNotifyGive(handle);
	}

	const uint32_t startedMs = millis();
	while (true) {
		{
			SignalLock lock(_impl->mutex);
			if (lock && _impl->taskHandle == nullptr) {
				_impl->initialized = false;
				_impl->stopping = false;
				_impl->queue.clear();
				_impl->subscriptions.clear();
				_impl->waiters.clear();
				_impl->queueHead = 0;
				_impl->queueCount = 0;
				return SignalResult::success("signal ended");
			}
		}
		if (timeoutElapsed(startedMs, timeoutMs)) {
			return SignalResult::failure(SignalStatus::Timeout, "signal end timed out");
		}
		vTaskDelay(pdMS_TO_TICKS(kWaitPollMs));
	}
}

SignalSubResult Signal::subscribe(SignalEventId eventId, SignalCallback callback) {
	if (!callback) {
		return SignalSubResult::failure(SignalStatus::InvalidArgument, "callback is required");
	}
	return subscribeRaw(
	    eventId,
	    0,
	    [callback](const void *, size_t) {
		    callback();
	    }
	);
}

SignalSubResult Signal::subscribeRaw(
    SignalEventId eventId,
    size_t payloadSize,
    SignalRawCallback callback
) {
	if (!callback) {
		return SignalSubResult::failure(SignalStatus::InvalidArgument, "callback is required");
	}

	SignalLock lock(_impl->mutex);
	if (!lock) {
		return SignalSubResult::failure(SignalStatus::InternalError, "failed to lock signal");
	}
	if (!_impl->initialized || _impl->stopping) {
		return SignalSubResult::failure(SignalStatus::NotInitialized, "signal is not initialized");
	}
	if (payloadSize > _impl->config.maxPayloadSize) {
		return SignalSubResult::failure(SignalStatus::InvalidArgument, "payload is too large");
	}
	if (_impl->subscriptions.size() >= _impl->config.maxSubscriptions) {
		return SignalSubResult::failure(
		    SignalStatus::TooManySubscriptions,
		    "maximum subscriptions reached"
		);
	}

	const SignalSubscriptionId id = _impl->nextSubscriptionId++;
	SignalSubscriptionRecord record;
	record.id = id;
	record.eventId = eventId;
	record.payloadSize = payloadSize;
	record.callback = callback;
	_impl->subscriptions.push_back(record);
	return SignalSubResult::success(id, "signal subscription added");
}

SignalResult Signal::unsubscribe(SignalSubscriptionId id) {
	if (id == kInvalidSubscriptionId) {
		return SignalResult::failure(SignalStatus::InvalidArgument, "subscription id is required");
	}

	SignalLock lock(_impl->mutex);
	if (!lock) {
		return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
	}
	if (!_impl->initialized || _impl->stopping) {
		return SignalResult::failure(SignalStatus::NotInitialized, "signal is not initialized");
	}

	const auto oldSize = _impl->subscriptions.size();
	_impl->subscriptions.erase(
	    std::remove_if(
	        _impl->subscriptions.begin(),
	        _impl->subscriptions.end(),
	        [id](const SignalSubscriptionRecord &record) {
		        return record.id == id;
	        }
	    ),
	    _impl->subscriptions.end()
	);
	if (_impl->subscriptions.size() == oldSize) {
		return SignalResult::failure(SignalStatus::SubscriptionNotFound, "subscription not found");
	}
	return SignalResult::success("signal subscription removed");
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
	if (payloadSize > 0 && payload == nullptr) {
		return SignalResult::failure(SignalStatus::InvalidArgument, "payload is required");
	}

	const uint32_t startedMs = millis();
	bool countedTimeoutDrop = false;

	while (true) {
		TaskHandle_t handle = nullptr;
		SignalResult result = SignalResult::success("signal event queued");

		{
			SignalLock lock(_impl->mutex);
			if (!lock) {
				return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
			}
			if (!_impl->initialized || _impl->stopping) {
				return SignalResult::failure(SignalStatus::NotInitialized, "signal is not initialized");
			}
			if (payloadSize > _impl->config.maxPayloadSize) {
				return SignalResult::failure(SignalStatus::InvalidArgument, "payload is too large");
			}

			const uint32_t effectiveTimeout =
			    useDefaultTimeout ? _impl->config.defaultPostTimeoutMs : timeoutMs;

			if (_impl->queueCount < _impl->queue.size()) {
				_impl->enqueueLocked(eventId, payloadSize, payload);
				_impl->completeWaitersLocked(
				    eventId,
				    payloadSize,
				    payload,
				    SignalStatus::Ok,
				    "signal event received"
				);
				_impl->postedCount++;
				handle = _impl->taskHandle;
			} else if (_impl->config.overflowPolicy == SignalOverflowPolicy::DropNewest) {
				_impl->droppedCount++;
				return SignalResult::failure(SignalStatus::QueueFull, "signal queue is full");
			} else if (_impl->config.overflowPolicy == SignalOverflowPolicy::DropOldest) {
				_impl->queueHead = (_impl->queueHead + 1) % _impl->queue.size();
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
				handle = _impl->taskHandle;
			} else if (effectiveTimeout == 0 || timeoutElapsed(startedMs, effectiveTimeout)) {
				if (!countedTimeoutDrop) {
					_impl->droppedCount++;
					countedTimeoutDrop = true;
				}
				return SignalResult::failure(SignalStatus::Timeout, "signal queue is full");
			} else {
				result = SignalResult::failure(SignalStatus::Busy, "signal queue is full");
			}

			if (handle != nullptr) {
				xTaskNotifyGive(handle);
			}
			if (result.status != SignalStatus::Busy) {
				return result;
			}
		}

		vTaskDelay(pdMS_TO_TICKS(kWaitPollMs));
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
	if (payloadSize > 0 && payloadOut == nullptr) {
		return SignalResult::failure(SignalStatus::InvalidArgument, "payload output is required");
	}

	SemaphoreHandle_t done = xSemaphoreCreateBinary();
	if (done == nullptr) {
		return SignalResult::failure(SignalStatus::OutOfMemory, "failed to create signal waiter");
	}

	SignalWaiterRecord waiter;
	waiter.eventId = eventId;
	waiter.payloadSize = payloadSize;
	waiter.payloadOut = payloadOut;
	waiter.done = done;

	{
		SignalLock lock(_impl->mutex);
		if (!lock) {
			vSemaphoreDelete(done);
			return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
		}
		if (!_impl->initialized || _impl->stopping) {
			vSemaphoreDelete(done);
			return SignalResult::failure(SignalStatus::NotInitialized, "signal is not initialized");
		}
		if (_impl->taskHandle != nullptr && xTaskGetCurrentTaskHandle() == _impl->taskHandle) {
			vSemaphoreDelete(done);
			return SignalResult::failure(
			    SignalStatus::InvalidArgument,
			    "waitFor cannot be called from the signal task"
			);
		}
		if (payloadSize > _impl->config.maxPayloadSize) {
			vSemaphoreDelete(done);
			return SignalResult::failure(SignalStatus::InvalidArgument, "payload is too large");
		}
		if (_impl->waiters.size() >= _impl->config.maxWaiters) {
			vSemaphoreDelete(done);
			return SignalResult::failure(SignalStatus::TooManyWaiters, "maximum waiters reached");
		}
		_impl->waiters.push_back(&waiter);
	}

	xSemaphoreTake(done, timeoutToTicks(timeoutMs));

	SignalResult result;
	{
		SignalLock lock(_impl->mutex);
		if (!lock) {
			vSemaphoreDelete(done);
			return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
		}
		_impl->removeWaiterLocked(&waiter);
		if (waiter.completed) {
			result = waiter.status == SignalStatus::Ok
			             ? SignalResult::success(waiter.message)
			             : SignalResult::failure(waiter.status, waiter.message);
		} else {
			result = SignalResult::failure(SignalStatus::Timeout, "signal wait timed out");
		}
	}

	vSemaphoreDelete(done);
	return result;
}

SignalDiag Signal::getDiagnostics() {
	SignalDiag diag;
	SignalLock lock(_impl->mutex);
	if (!lock) {
		return diag;
	}
	diag.postedCount = _impl->postedCount;
	diag.dispatchedCount = _impl->dispatchedCount;
	diag.droppedCount = _impl->droppedCount;
	diag.queueSize = _impl->queue.size();
	diag.queueUsed = _impl->queueCount;
	diag.subscriptionCount = _impl->subscriptions.size();
	diag.waiterCount = _impl->waiters.size();
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
