#include "fake_freertos.h"

#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

struct FakeSemaphore {
	enum class Kind {
		Recursive,
		Counting,
	};

	Kind kind = Kind::Counting;
	std::mutex mutex;
	std::condition_variable cv;
	uint32_t maxCount = 1;
	uint32_t count = 0;
	std::thread::id owner;
	uint32_t recursion = 0;
};

struct FakeTask {
	std::mutex mutex;
	std::condition_variable cv;
	uint32_t notifications = 0;
};

namespace {
thread_local FakeTask *currentTask = nullptr;
std::atomic<size_t> psramBytes{0};
std::atomic<uint32_t> failTaskCreates{0};
std::atomic<uint32_t> failCapsTaskCreates{0};
std::atomic<uint32_t> taskCreates{0};
std::atomic<uint32_t> capsTaskCreates{0};
std::atomic<uint32_t> taskCreateDelayMs{0};

bool consumeFailure(std::atomic<uint32_t> &counter) {
	uint32_t value = counter.load();
	while (value > 0) {
		if (counter.compare_exchange_weak(value, value - 1)) {
			return true;
		}
	}
	return false;
}

template <typename Predicate>
bool waitFor(
    std::unique_lock<std::mutex> &lock,
    std::condition_variable &cv,
    TickType_t timeout,
    Predicate predicate
) {
	if (predicate()) {
		return true;
	}
	if (timeout == 0) {
		return false;
	}
	if (timeout == portMAX_DELAY) {
		cv.wait(lock, predicate);
		return true;
	}
	return cv.wait_for(lock, std::chrono::milliseconds(timeout), predicate);
}

BaseType_t createTask(
    TaskFunction_t entry,
    void *arg,
    TaskHandle_t *handle,
    bool withCaps
) {
	if (handle == nullptr || entry == nullptr) {
		return pdFAIL;
	}
	if (withCaps) {
		capsTaskCreates.fetch_add(1);
		if (consumeFailure(failCapsTaskCreates)) {
			*handle = nullptr;
			return pdFAIL;
		}
	} else {
		taskCreates.fetch_add(1);
		if (consumeFailure(failTaskCreates)) {
			*handle = nullptr;
			return pdFAIL;
		}
	}

	const uint32_t delayMs = taskCreateDelayMs.load();
	if (delayMs > 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
	}

	auto *task = new FakeTask();
	*handle = task;
	std::thread([task, entry, arg]() {
		currentTask = task;
		entry(arg);
		currentTask = nullptr;
	}).detach();
	return pdPASS;
}
} // namespace

namespace fake_freertos {
void reset() {
	psramBytes.store(0);
	failTaskCreates.store(0);
	failCapsTaskCreates.store(0);
	taskCreates.store(0);
	capsTaskCreates.store(0);
	taskCreateDelayMs.store(0);
}

void setPsramBytes(size_t bytes) {
	psramBytes.store(bytes);
}

void failNextTaskCreates(uint32_t count) {
	failTaskCreates.store(count);
}

void failNextCapsTaskCreates(uint32_t count) {
	failCapsTaskCreates.store(count);
}

void setTaskCreateDelayMs(uint32_t milliseconds) {
	taskCreateDelayMs.store(milliseconds);
}

uint32_t taskCreateCount() {
	return taskCreates.load();
}

uint32_t capsTaskCreateCount() {
	return capsTaskCreates.load();
}
} // namespace fake_freertos

extern "C" {
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) {
	auto *semaphore = new FakeSemaphore();
	semaphore->kind = FakeSemaphore::Kind::Recursive;
	return semaphore;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void) {
	auto *semaphore = new FakeSemaphore();
	semaphore->kind = FakeSemaphore::Kind::Counting;
	semaphore->maxCount = 1;
	semaphore->count = 0;
	return semaphore;
}

SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t maxCount, UBaseType_t initialCount) {
	if (maxCount == 0 || initialCount > maxCount) {
		return nullptr;
	}
	auto *semaphore = new FakeSemaphore();
	semaphore->kind = FakeSemaphore::Kind::Counting;
	semaphore->maxCount = maxCount;
	semaphore->count = initialCount;
	return semaphore;
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t handle, TickType_t timeout) {
	if (handle == nullptr || handle->kind != FakeSemaphore::Kind::Recursive) {
		return pdFALSE;
	}
	std::unique_lock lock(handle->mutex);
	const auto caller = std::this_thread::get_id();
	if (handle->recursion > 0 && handle->owner == caller) {
		handle->recursion++;
		return pdTRUE;
	}
	if (!waitFor(lock, handle->cv, timeout, [&]() { return handle->recursion == 0; })) {
		return pdFALSE;
	}
	handle->owner = caller;
	handle->recursion = 1;
	return pdTRUE;
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t handle) {
	if (handle == nullptr || handle->kind != FakeSemaphore::Kind::Recursive) {
		return pdFALSE;
	}
	std::lock_guard lock(handle->mutex);
	if (handle->recursion == 0 || handle->owner != std::this_thread::get_id()) {
		return pdFALSE;
	}
	handle->recursion--;
	if (handle->recursion == 0) {
		handle->owner = std::thread::id();
		handle->cv.notify_one();
	}
	return pdTRUE;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t timeout) {
	if (handle == nullptr || handle->kind != FakeSemaphore::Kind::Counting) {
		return pdFALSE;
	}
	std::unique_lock lock(handle->mutex);
	if (!waitFor(lock, handle->cv, timeout, [&]() { return handle->count > 0; })) {
		return pdFALSE;
	}
	handle->count--;
	return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle) {
	if (handle == nullptr || handle->kind != FakeSemaphore::Kind::Counting) {
		return pdFALSE;
	}
	std::lock_guard lock(handle->mutex);
	if (handle->count >= handle->maxCount) {
		return pdFALSE;
	}
	handle->count++;
	handle->cv.notify_one();
	return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t handle) {
	delete handle;
}

BaseType_t xTaskCreate(
    TaskFunction_t entry,
    const char *,
    uint32_t,
    void *arg,
    UBaseType_t,
    TaskHandle_t *handle
) {
	return createTask(entry, arg, handle, false);
}

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t entry,
    const char *,
    uint32_t,
    void *arg,
    UBaseType_t,
    TaskHandle_t *handle,
    BaseType_t
) {
	return createTask(entry, arg, handle, false);
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(
    TaskFunction_t entry,
    const char *,
    configSTACK_DEPTH_TYPE,
    void *arg,
    UBaseType_t,
    TaskHandle_t *handle,
    BaseType_t,
    UBaseType_t
) {
	return createTask(entry, arg, handle, true);
}

TaskHandle_t xTaskGetCurrentTaskHandle(void) {
	return currentTask;
}

BaseType_t xTaskNotifyGive(TaskHandle_t handle) {
	if (handle == nullptr) {
		return pdFAIL;
	}
	std::lock_guard lock(handle->mutex);
	handle->notifications++;
	handle->cv.notify_one();
	return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clearCountOnExit, TickType_t timeout) {
	if (currentTask == nullptr) {
		return 0;
	}
	std::unique_lock lock(currentTask->mutex);
	if (!waitFor(lock, currentTask->cv, timeout, [&]() { return currentTask->notifications > 0; })) {
		return 0;
	}
	const uint32_t result = currentTask->notifications;
	if (clearCountOnExit == pdTRUE) {
		currentTask->notifications = 0;
	} else {
		currentTask->notifications--;
	}
	return result;
}

void vTaskDelay(TickType_t ticks) {
	std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}

void vTaskDelete(TaskHandle_t) {
}

void vTaskDeleteWithCaps(TaskHandle_t) {
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) {
	return 1024;
}

size_t heap_caps_get_total_size(uint32_t caps) {
	return (caps & MALLOC_CAP_SPIRAM) != 0 ? psramBytes.load() : 0;
}
}
