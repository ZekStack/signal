#include "Signal.h"

#include <strata/freertos/BinarySemaphore.h>
#include <strata/freertos/CountingSemaphore.h>
#include <strata/freertos/Mutex.h>
#include <strata/freertos/Task.h>

#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

namespace zek::signal {
namespace {
constexpr SignalSubscriptionId kInvalidSubscriptionId = 0;
constexpr size_t kMinStackSizeBytes = 1024;

enum class SignalLifecycleState : uint8_t {
	Stopped,
	Initializing,
	Running,
	Stopping,
};

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

bool isValidStackSize(size_t stackBytes) {
	return stackBytes >= kMinStackSizeBytes && (stackBytes % sizeof(StackType_t)) == 0;
}

SignalResult allocationFailure() {
	return SignalResult::failure(SignalStatus::OutOfMemory, "signal allocation failed");
}

SignalSubResult subscriptionAllocationFailure() {
	return SignalSubResult::failure(SignalStatus::OutOfMemory, "signal allocation failed");
}

class SignalLock {
  public:
	explicit SignalLock(Strata::FreeRTOS::RecursiveMutex &mutex)
	    : _mutex(mutex), _locked(mutex.lock()) {
	}

	~SignalLock() {
		if (_locked) {
			_mutex.unlock();
		}
	}

	SignalLock(const SignalLock &) = delete;
	SignalLock &operator=(const SignalLock &) = delete;

	explicit operator bool() const {
		return _locked;
	}

  private:
	Strata::FreeRTOS::RecursiveMutex &_mutex;
	bool _locked = false;
};

[[noreturn]] void suspendForever() {
	vTaskSuspend(nullptr);
	for (;;) {
		vTaskDelay(portMAX_DELAY);
	}
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
	Strata::FreeRTOS::BinarySemaphore done;
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

template <typename T>
class SignalArray {
	static_assert(std::is_nothrow_default_constructible_v<T>);
	static_assert(std::is_nothrow_destructible_v<T>);

  public:
	SignalArray() noexcept = default;

	~SignalArray() noexcept {
		reset();
	}

	SignalArray(const SignalArray &) = delete;
	SignalArray &operator=(const SignalArray &) = delete;

	[[nodiscard]] bool allocate(
	    size_t count,
	    Strata::Placement placement
	) noexcept {
		reset();
		if (count == 0) {
			return true;
		}
		if (count > std::numeric_limits<size_t>::max() / sizeof(T)) {
			return false;
		}

		Strata::Buffer storage(count * sizeof(T), placement);
		auto *data = storage.data<T>();
		if (data == nullptr) {
			return false;
		}

		for (size_t i = 0; i < count; ++i) {
			std::construct_at(data + i);
		}

		storage_ = std::move(storage);
		size_ = count;
		return true;
	}

	void reset() noexcept {
		auto *items = data();
		for (size_t i = size_; i > 0; --i) {
			std::destroy_at(items + (i - 1));
		}
		size_ = 0;
		storage_.reset();
	}

	[[nodiscard]] T *data() noexcept {
		return storage_.data<T>();
	}

	[[nodiscard]] const T *data() const noexcept {
		return storage_.data<T>();
	}

	[[nodiscard]] size_t size() const noexcept {
		return size_;
	}

	[[nodiscard]] bool empty() const noexcept {
		return size_ == 0;
	}

	[[nodiscard]] explicit operator bool() const noexcept {
		return data() != nullptr;
	}

	T &operator[](size_t index) noexcept {
		return data()[index];
	}

	const T &operator[](size_t index) const noexcept {
		return data()[index];
	}

	T *begin() noexcept {
		return data();
	}

	T *end() noexcept {
		return data() + size_;
	}

  private:
	Strata::Buffer storage_;
	size_t size_ = 0;
};

struct SignalImpl {
	SignalImpl() noexcept : mutex(Strata::FreeRTOS::RecursiveMutex::create()) {
	}

	SignalConfig config{};
	Strata::FreeRTOS::RecursiveMutex mutex;
	SignalArray<SignalQueuedEvent> queue;
	Strata::Buffer queuePayloadStorage;
	Strata::Buffer dispatchPayload;
	SignalArray<SignalDispatchMatch> dispatchMatches;
	size_t queueHead = 0;
	size_t queueCount = 0;
	SignalArray<SignalSubscriptionRecord> subscriptions;
	size_t subscriptionCapacity = 0;
	size_t activeSubscriptionCount = 0;
	SignalArray<SignalWaiterRecord> waiters;
	size_t waiterCapacity = 0;
	size_t activeWaiterCount = 0;
	size_t activePostOperations = 0;
	Strata::FreeRTOS::CountingSemaphore queueSpace;
	SignalLifecycleState lifecycle = SignalLifecycleState::Stopped;
	Strata::FreeRTOS::Task task;
	std::atomic<bool> taskReadyForDelete{false};
	uint64_t nextSequence = 1;
	SignalSubscriptionId nextSubscriptionId = 1;
	uint32_t postedCount = 0;
	uint32_t processedEventCount = 0;
	uint32_t callbackInvokeCount = 0;
	uint32_t droppedCount = 0;
	uint32_t rejectedCount = 0;
	uint32_t dispatchErrorCount = 0;
	size_t stackHighWaterMarkBytes = 0;

	bool isRunningLocked() const {
		return lifecycle == SignalLifecycleState::Running;
	}

	bool isStoppingLocked() const {
		return lifecycle == SignalLifecycleState::Stopping;
	}

	bool canCleanupLocked() const {
		return taskReadyForDelete.load(std::memory_order_acquire) &&
		       activeWaiterCount == 0 && activePostOperations == 0;
	}

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
		queueSpace.reset();
		waiters.reset();
		subscriptions.reset();
		dispatchMatches.reset();
		dispatchPayload.reset();
		queuePayloadStorage.reset();
		queue.reset();
		subscriptionCapacity = 0;
		activeSubscriptionCount = 0;
		waiterCapacity = 0;
		activeWaiterCount = 0;
		activePostOperations = 0;
		queueHead = 0;
		queueCount = 0;
	}

	void resetRuntimeStateLocked() {
		lifecycle = SignalLifecycleState::Stopped;
		taskReadyForDelete.store(false, std::memory_order_release);
		queueHead = 0;
		queueCount = 0;
		activeSubscriptionCount = 0;
		activeWaiterCount = 0;
		activePostOperations = 0;
		resetCounters();
	}

	void cleanupAfterFailedInitLocked() {
		cleanupStorage();
		resetRuntimeStateLocked();
	}

	bool allocateStorageLocked(const SignalConfig &newConfig) {
		cleanupStorage();
		const Strata::Placement placement = newConfig.memory.allocation;

		if (!queue.allocate(newConfig.queueSize, placement)) {
			cleanupStorage();
			return false;
		}

		const size_t payloadBytes = newConfig.queueSize * newConfig.maxPayloadSize;
		if (payloadBytes > 0) {
			queuePayloadStorage = Strata::Buffer(payloadBytes, placement);
			if (queuePayloadStorage.data() == nullptr) {
				cleanupStorage();
				return false;
			}
		}
		auto *queuePayload = queuePayloadStorage.data<uint8_t>();
		for (size_t i = 0; i < newConfig.queueSize; ++i) {
			queue[i].payload = queuePayload != nullptr
			                       ? queuePayload + (i * newConfig.maxPayloadSize)
			                       : nullptr;
		}

		if (newConfig.maxPayloadSize > 0) {
			dispatchPayload = Strata::Buffer(newConfig.maxPayloadSize, placement);
			if (dispatchPayload.data() == nullptr) {
				cleanupStorage();
				return false;
			}
		}

		if (!dispatchMatches.allocate(newConfig.maxSubscriptions, placement) ||
		    !subscriptions.allocate(newConfig.maxSubscriptions, placement)) {
			cleanupStorage();
			return false;
		}
		subscriptionCapacity = newConfig.maxSubscriptions;

		if (newConfig.maxWaiters > 0) {
			if (!waiters.allocate(newConfig.maxWaiters, placement)) {
				cleanupStorage();
				return false;
			}
			waiterCapacity = newConfig.maxWaiters;
			for (auto &waiter : waiters) {
				waiter.done = Strata::FreeRTOS::BinarySemaphore::create();
				if (!waiter.done) {
					cleanupStorage();
					return false;
				}
			}
		}

		queueSpace = Strata::FreeRTOS::CountingSemaphore::create(
		    newConfig.queueSize,
		    newConfig.queueSize
		);
		if (!queueSpace) {
			cleanupStorage();
			return false;
		}

		return true;
	}

	void notifyTaskLocked() const {
		if (task) {
			xTaskNotifyGive(task.handle());
		}
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
		if (queueCount == 0 || !queue) {
			return false;
		}

		SignalQueuedEvent &slot = queue[queueHead];
		event.eventId = slot.eventId;
		event.payloadSize = slot.payloadSize;
		event.sequence = slot.sequence;
		event.payload = slot.payloadSize > 0 ? dispatchPayload.data<uint8_t>() : nullptr;
		if (slot.payloadSize > 0) {
			memcpy(dispatchPayload.data(), slot.payload, slot.payloadSize);
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
		if (!waiters) {
			return;
		}
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
			(void)waiter.done.give();
		}
	}

	void failAllWaitersLocked(SignalStatus status, const char *message) {
		if (!waiters) {
			return;
		}
		for (size_t i = 0; i < waiterCapacity; ++i) {
			SignalWaiterRecord &waiter = waiters[i];
			if (!waiter.inUse || waiter.completed) {
				continue;
			}
			waiter.status = status;
			waiter.message = message != nullptr ? message : "signal stopped";
			waiter.completed = true;
			(void)waiter.done.give();
		}
	}

	SignalWaiterRecord *findFreeWaiterLocked() {
		if (!waiters) {
			return nullptr;
		}
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
		if (!subscriptions) {
			return nullptr;
		}
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
		if (!subscriptions || index >= subscriptionCapacity) {
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
		if (!subscriptions || !dispatchMatches) {
			return 0;
		}
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

	void observeStackHighWaterLocked() {
		const size_t current = task ? task.stackHighWaterMarkBytes() : 0;
		if (current > 0 && (stackHighWaterMarkBytes == 0 || current < stackHighWaterMarkBytes)) {
			stackHighWaterMarkBytes = current;
		}
	}

	bool popNext(SignalDispatchEvent &event) {
		SignalLock lock(mutex);
		if (!lock || !popLocked(event)) {
			return false;
		}
		if (config.overflowPolicy == SignalOverflowPolicy::BlockCaller && queueSpace) {
			(void)queueSpace.give();
		}
		return true;
	}

	bool isStoppingAndEmpty() {
		SignalLock lock(mutex);
		return lock && isStoppingLocked() && queueCount == 0;
	}

	void markTaskStopped() {
		{
			SignalLock lock(mutex);
			if (lock) {
				observeStackHighWaterLocked();
			}
		}
		taskReadyForDelete.store(true, std::memory_order_release);
	}

	static void taskEntry(void *arg) {
		SignalImpl *impl = static_cast<SignalImpl *>(arg);
		if (impl == nullptr) {
			suspendForever();
		}

		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

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
					const SignalDispatchMatch match = (*impl->dispatchMatches)[i];
					bool invoked = false;
					SignalCallbackKind kind = SignalCallbackKind::None;
					SignalRawCallback rawCallback = nullptr;
					SignalRawPayloadCallback rawPayloadCallback = nullptr;
					void *context = nullptr;

					{
						SignalLock lock(impl->mutex);
						if (!lock || !impl->subscriptions ||
						    match.index >= impl->subscriptionCapacity) {
							if (lock) {
								impl->dispatchErrorCount++;
							}
							continue;
						}
						SignalSubscriptionRecord &slot = (*impl->subscriptions)[match.index];
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
							if (lock && impl->subscriptions &&
							    match.index < impl->subscriptionCapacity) {
								SignalSubscriptionRecord &candidate =
								    (*impl->subscriptions)[match.index];
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
						impl->observeStackHighWaterLocked();
					}
				}
				continue;
			}

			if (impl->isStoppingAndEmpty()) {
				break;
			}

			{
				SignalLock lock(impl->mutex);
				if (lock) {
					impl->observeStackHighWaterLocked();
				}
			}
			ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		}

		impl->markTaskStopped();
		suspendForever();
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

Signal::Signal() : _impl(Strata::makeUnique<SignalImpl>(Strata::Placement::Internal)) {
}

Signal::~Signal() {
	if (_impl == nullptr) {
		return;
	}
	if (_impl->task && xTaskGetCurrentTaskHandle() == _impl->task.handle()) {
		// Signal destruction from its own callback cannot safely reclaim the task-owned
		// static stack. Preserve the existing fail-safe leak behavior for this unsupported case.
		_impl.release();
		return;
	}
	end(portMAX_DELAY);
}

SignalResult Signal::init(const SignalConfig &config) {
	if (_impl == nullptr) {
		return allocationFailure();
	}
	if (!_impl->mutex) {
		return SignalResult::failure(SignalStatus::OutOfMemory, "failed to allocate signal mutex");
	}
	if (!Strata::validMemoryPolicy(config.memory)) {
		return SignalResult::failure(SignalStatus::InvalidArgument, "invalid memory placement");
	}
	if (!isValidStackSize(config.stackSizeBytes)) {
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
	if (
	    config.maxPayloadSize > 0 &&
	    config.queueSize > std::numeric_limits<size_t>::max() / config.maxPayloadSize
	) {
		return SignalResult::failure(
		    SignalStatus::InvalidArgument,
		    "queue payload storage size overflow"
		);
	}
	if (config.queueSize > static_cast<size_t>(std::numeric_limits<UBaseType_t>::max())) {
		return SignalResult::failure(
		    SignalStatus::InvalidArgument,
		    "queue size is too large for FreeRTOS semaphore"
		);
	}

	{
		SignalLock lock(_impl->mutex);
		if (!lock) {
			return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
		}
		switch (_impl->lifecycle) {
		case SignalLifecycleState::Running:
			return SignalResult::failure(SignalStatus::AlreadyInitialized, "signal already initialized");
		case SignalLifecycleState::Initializing:
		case SignalLifecycleState::Stopping:
			return SignalResult::failure(SignalStatus::Busy, "signal lifecycle transition in progress");
		case SignalLifecycleState::Stopped:
			break;
		}
		_impl->lifecycle = SignalLifecycleState::Initializing;
		_impl->taskReadyForDelete.store(false, std::memory_order_release);
		if (!_impl->allocateStorageLocked(config)) {
			_impl->cleanupAfterFailedInitLocked();
			return SignalResult::failure(SignalStatus::OutOfMemory, "failed to allocate signal storage");
		}
		_impl->config = config;
		_impl->resetCounters();
	}

	auto task = Strata::FreeRTOS::Task::create(
	    &SignalImpl::taskEntry,
	    _impl.get(),
	    Strata::FreeRTOS::TaskConfig{
	        .name = config.taskName,
	        .stackBytes = config.stackSizeBytes,
	        .stackPlacement = config.memory.taskStack,
	        .priority = config.priority,
	        .affinity = config.coreId,
	    }
	);
	if (!task) {
		SignalLock lock(_impl->mutex);
		if (lock) {
			_impl->cleanupAfterFailedInitLocked();
		}
		return SignalResult::failure(SignalStatus::TaskCreateFailed, "failed to create signal task");
	}

	{
		SignalLock lock(_impl->mutex);
		if (!lock) {
			task.reset();
			return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
		}
		_impl->task = std::move(task);
		_impl->lifecycle = SignalLifecycleState::Running;
		xTaskNotifyGive(_impl->task.handle());
	}

	return SignalResult::success("signal initialized");
}

SignalResult Signal::end(uint32_t timeoutMs) {
	if (_impl == nullptr) {
		return allocationFailure();
	}

	{
		SignalLock lock(_impl->mutex);
		if (!lock) {
			return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
		}
		if (_impl->lifecycle == SignalLifecycleState::Stopped) {
			return SignalResult::success("signal is not initialized");
		}
		if (_impl->lifecycle == SignalLifecycleState::Initializing) {
			return SignalResult::failure(SignalStatus::Busy, "signal initialization is in progress");
		}
		if (_impl->task && xTaskGetCurrentTaskHandle() == _impl->task.handle()) {
			return SignalResult::failure(
			    SignalStatus::InvalidArgument,
			    "end cannot be called from the signal task"
			);
		}
		if (_impl->lifecycle == SignalLifecycleState::Running) {
			_impl->lifecycle = SignalLifecycleState::Stopping;
			_impl->failAllWaitersLocked(SignalStatus::NotInitialized, "signal is ending");
			_impl->notifyTaskLocked();
		}
	}

	const uint32_t startedMs = millis();
	while (true) {
		{
			SignalLock lock(_impl->mutex);
			if (lock && _impl->lifecycle == SignalLifecycleState::Stopped) {
				return SignalResult::success("signal ended");
			}
			if (lock && _impl->isStoppingLocked() && _impl->canCleanupLocked()) {
				if (_impl->task) {
					vTaskSuspend(_impl->task.handle());
					_impl->task.reset();
				}
				_impl->cleanupStorage();
				_impl->lifecycle = SignalLifecycleState::Stopped;
				_impl->taskReadyForDelete.store(false, std::memory_order_release);
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
	if (!_impl->isRunningLocked()) {
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
	if (!_impl->isRunningLocked()) {
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
	if (!_impl->isRunningLocked()) {
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
	if (!_impl->isRunningLocked()) {
		_impl->rejectedCount++;
		return SignalResult::failure(SignalStatus::NotInitialized, "signal is not initialized");
	}

	for (size_t i = 0; i < _impl->subscriptionCapacity; ++i) {
		SignalSubscriptionRecord &slot = (*_impl->subscriptions)[i];
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

	uint32_t effectiveTimeout = timeoutMs;
	SignalOverflowPolicy overflowPolicy = SignalOverflowPolicy::DropNewest;
	Strata::FreeRTOS::CountingSemaphore *queueSpace = nullptr;
	bool calledFromSignalTask = false;
	{
		SignalLock lock(_impl->mutex);
		if (!lock) {
			return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
		}
		if (!_impl->isRunningLocked()) {
			_impl->rejectedCount++;
			return SignalResult::failure(SignalStatus::NotInitialized, "signal is not initialized");
		}
		if (payloadSize > _impl->config.maxPayloadSize) {
			_impl->rejectedCount++;
			return SignalResult::failure(SignalStatus::InvalidArgument, "payload is too large");
		}
		effectiveTimeout = useDefaultTimeout ? _impl->config.defaultPostTimeoutMs : timeoutMs;
		overflowPolicy = _impl->config.overflowPolicy;

		if (overflowPolicy != SignalOverflowPolicy::BlockCaller) {
			if (_impl->queueCount >= _impl->config.queueSize) {
				if (overflowPolicy == SignalOverflowPolicy::DropNewest) {
					_impl->droppedCount++;
					_impl->rejectedCount++;
					return SignalResult::failure(SignalStatus::QueueFull, "signal queue is full");
				}
				_impl->queueHead = (_impl->queueHead + 1) % _impl->config.queueSize;
				_impl->queueCount--;
				_impl->droppedCount++;
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
			_impl->notifyTaskLocked();
			return SignalResult::success("signal event queued");
		}

		queueSpace = &_impl->queueSpace;
		if (!*queueSpace) {
			_impl->dispatchErrorCount++;
			return SignalResult::failure(
			    SignalStatus::InternalError,
			    "signal queue space is unavailable"
			);
		}
		calledFromSignalTask =
		    _impl->task && xTaskGetCurrentTaskHandle() == _impl->task.handle();
		_impl->activePostOperations++;
	}

	const TickType_t waitTicks = calledFromSignalTask ? 0 : timeoutToTicks(effectiveTimeout);
	if (!queueSpace->take(waitTicks)) {
		SignalLock lock(_impl->mutex);
		if (lock) {
			_impl->droppedCount++;
			_impl->rejectedCount++;
			if (_impl->activePostOperations > 0) {
				_impl->activePostOperations--;
			}
		}
		if (calledFromSignalTask) {
			return SignalResult::failure(
			    SignalStatus::Busy,
			    "blocking post cannot wait from the signal task"
			);
		}
		return SignalResult::failure(SignalStatus::Timeout, "signal queue is full");
	}

	SignalResult result;
	{
		SignalLock lock(_impl->mutex);
		if (!lock) {
			(void)queueSpace->give();
			return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
		}
		if (!_impl->isRunningLocked()) {
			(void)queueSpace->give();
			_impl->rejectedCount++;
			result = SignalResult::failure(SignalStatus::NotInitialized, "signal is not initialized");
		} else if (payloadSize > _impl->config.maxPayloadSize) {
			(void)queueSpace->give();
			_impl->rejectedCount++;
			result = SignalResult::failure(SignalStatus::InvalidArgument, "payload is too large");
		} else if (_impl->queueCount >= _impl->config.queueSize) {
			(void)queueSpace->give();
			_impl->dispatchErrorCount++;
			result = SignalResult::failure(
			    SignalStatus::InternalError,
			    "signal queue reservation failed"
			);
		} else {
			_impl->enqueueLocked(eventId, payloadSize, payload);
			_impl->completeWaitersLocked(
			    eventId,
			    payloadSize,
			    payload,
			    SignalStatus::Ok,
			    "signal event received"
			);
			_impl->postedCount++;
			_impl->notifyTaskLocked();
			result = SignalResult::success("signal event queued");
		}
		if (_impl->activePostOperations > 0) {
			_impl->activePostOperations--;
		}
	}
	return result;
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
	Strata::FreeRTOS::BinarySemaphore *done = nullptr;
	{
		SignalLock lock(_impl->mutex);
		if (!lock) {
			return SignalResult::failure(SignalStatus::InternalError, "failed to lock signal");
		}
		if (!_impl->isRunningLocked()) {
			_impl->rejectedCount++;
			return SignalResult::failure(SignalStatus::NotInitialized, "signal is not initialized");
		}
		if (_impl->task && xTaskGetCurrentTaskHandle() == _impl->task.handle()) {
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
		(void)waiter->done.tryTake();
		waiter->eventId = eventId;
		waiter->payloadSize = payloadSize;
		waiter->payloadOut = payloadOut;
		waiter->inUse = true;
		waiter->completed = false;
		waiter->status = SignalStatus::Timeout;
		waiter->message = "signal wait timed out";
		_impl->activeWaiterCount++;
		done = &waiter->done;
	}

	(void)done->take(timeoutToTicks(timeoutMs));

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
	diag.stackHighWaterMarkBytes =
	    _impl->task ? _impl->task.stackHighWaterMarkBytes() : _impl->stackHighWaterMarkBytes;
	diag.taskStackPlacement =
	    _impl->task ? _impl->task.stackPlacement() : _impl->config.memory.taskStack;
	diag.taskStackRegion =
	    _impl->task ? _impl->task.stackRegion() : Strata::Region::Unknown;
	diag.allocationPlacement = _impl->config.memory.allocation;
	diag.queueStorageRegion =
	    !_impl->queue.empty()
	        ? Strata::regionOf(_impl->queue.data())
	        : Strata::Region::Unknown;
	diag.queuePayloadRegion = _impl->queuePayloadStorage.region();
	diag.dispatchPayloadRegion = _impl->dispatchPayload.region();
	diag.dispatchMatchStorageRegion =
	    !_impl->dispatchMatches.empty()
	        ? Strata::regionOf(_impl->dispatchMatches.data())
	        : Strata::Region::Unknown;
	diag.subscriptionStorageRegion =
	    !_impl->subscriptions.empty()
	        ? Strata::regionOf(_impl->subscriptions.data())
	        : Strata::Region::Unknown;
	diag.waiterStorageRegion =
	    !_impl->waiters.empty()
	        ? Strata::regionOf(_impl->waiters.data())
	        : Strata::Region::Unknown;
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
