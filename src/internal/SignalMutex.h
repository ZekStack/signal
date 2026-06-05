#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class SignalMutex {
  public:
	SignalMutex() {
		_handle = xSemaphoreCreateRecursiveMutex();
	}

	~SignalMutex() {
		if (_handle != nullptr) {
			vSemaphoreDelete(_handle);
		}
	}

	SignalMutex(const SignalMutex &) = delete;
	SignalMutex &operator=(const SignalMutex &) = delete;

	bool lock(TickType_t timeout = portMAX_DELAY) {
		return _handle != nullptr && xSemaphoreTakeRecursive(_handle, timeout) == pdTRUE;
	}

	void unlock() {
		if (_handle != nullptr) {
			xSemaphoreGiveRecursive(_handle);
		}
	}

  private:
	SemaphoreHandle_t _handle = nullptr;
};

class SignalLock {
  public:
	explicit SignalLock(SignalMutex &mutex) : _mutex(mutex), _locked(mutex.lock()) {
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
	SignalMutex &_mutex;
	bool _locked = false;
};
