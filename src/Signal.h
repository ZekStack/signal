#pragma once

#include <Arduino.h>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

using SignalEventId = uint32_t;
using SignalSubscriptionId = uint32_t;

struct SignalImpl;

enum class SignalStatus : uint8_t {
	Ok,
	NotInitialized,
	AlreadyInitialized,
	InvalidArgument,
	OutOfMemory,
	TaskCreateFailed,
	SubscriptionNotFound,
	QueueFull,
	TooManySubscriptions,
	TooManyWaiters,
	Busy,
	Timeout,
	InternalError,
};

enum class SignalStackType : uint8_t {
	Auto,
	Internal,
	Psram,
};

enum class SignalOverflowPolicy : uint8_t {
	DropNewest,
	DropOldest,
	BlockCaller,
};

struct SignalConfig {
	uint32_t stackSizeBytes = 4096;
	UBaseType_t priority = 1;
	BaseType_t coreId = tskNO_AFFINITY;
	SignalStackType stackType = SignalStackType::Auto;
	size_t queueSize = 20;
	size_t maxPayloadSize = 128;
	size_t maxSubscriptions = 32;
	size_t maxWaiters = 8;
	SignalOverflowPolicy overflowPolicy = SignalOverflowPolicy::DropNewest;
	uint32_t defaultPostTimeoutMs = 0;
	const char *taskName = "signal-task";
};

struct SignalResult {
	bool result = false;
	SignalStatus status = SignalStatus::InternalError;
	std::string message;

	explicit operator bool() const {
		return result;
	}

	static SignalResult success(const char *message = "ok");
	static SignalResult failure(SignalStatus status, const char *message);
};

struct SignalSubResult : SignalResult {
	SignalSubscriptionId id = 0;

	static SignalSubResult success(SignalSubscriptionId id, const char *message = "ok");
	static SignalSubResult failure(
	    SignalStatus status,
	    const char *message,
	    SignalSubscriptionId id = 0
	);
};

struct SignalDiag {
	uint32_t postedCount = 0;
	uint32_t dispatchedCount = 0;
	uint32_t droppedCount = 0;
	size_t queueSize = 0;
	size_t queueUsed = 0;
	size_t subscriptionCount = 0;
	size_t waiterCount = 0;
	uint32_t dispatchErrorCount = 0;
	size_t stackHighWaterMarkBytes = 0;
	SignalStackType requestedStackType = SignalStackType::Auto;
	SignalStackType actualStackType = SignalStackType::Internal;
};

using SignalCallback = std::function<void()>;

namespace signal_detail {
template <typename T>
struct IsSignalEventType {
	using Value = std::remove_cv_t<std::remove_reference_t<T>>;
	static constexpr bool value =
	    (std::is_enum_v<Value> || std::is_integral_v<Value>) && !std::is_same_v<Value, bool> &&
	    sizeof(Value) <= sizeof(SignalEventId);
};

template <typename T> SignalEventId eventToId(T event) {
	static_assert(IsSignalEventType<T>::value, "Signal event IDs must be enum or integral <= 32 bits");
	using Value = std::remove_cv_t<std::remove_reference_t<T>>;
	if constexpr (std::is_enum_v<Value>) {
		using Underlying = std::underlying_type_t<Value>;
		static_assert(sizeof(Underlying) <= sizeof(SignalEventId), "Signal enum IDs must fit uint32_t");
		return static_cast<SignalEventId>(static_cast<Underlying>(event));
	} else {
		return static_cast<SignalEventId>(event);
	}
}
} // namespace signal_detail

class Signal {
  public:
	Signal();
	~Signal();

	Signal(const Signal &) = delete;
	Signal &operator=(const Signal &) = delete;

	SignalResult init(const SignalConfig &config = SignalConfig());
	SignalResult end(uint32_t timeoutMs = 5000);

	SignalSubResult subscribe(SignalEventId eventId, SignalCallback callback);

	template <
	    typename Event,
	    typename std::enable_if_t<
	        signal_detail::IsSignalEventType<Event>::value &&
	            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Event>>, SignalEventId>,
	        int> = 0>
	SignalSubResult subscribe(Event event, SignalCallback callback) {
		return subscribe(signal_detail::eventToId(event), callback);
	}

	template <typename Payload, typename Event, typename Callback>
	SignalSubResult subscribe(Event event, Callback callback) {
		using PayloadType = std::remove_cv_t<std::remove_reference_t<Payload>>;
		static_assert(signal_detail::IsSignalEventType<Event>::value, "Invalid Signal event ID type");
		static_assert(
		    std::is_trivially_copyable_v<PayloadType>,
		    "Signal payloads must be trivially copyable"
		);

		std::function<void(const PayloadType &)> typedCallback = callback;
		if (!typedCallback) {
			return SignalSubResult::failure(
			    SignalStatus::InvalidArgument,
			    "callback is required"
			);
		}

		return subscribeRaw(
		    signal_detail::eventToId(event),
		    sizeof(PayloadType),
		    [typedCallback](const void *data, size_t size) {
			    if (data == nullptr || size != sizeof(PayloadType)) {
				    return;
			    }
			    PayloadType payload;
			    memcpy(&payload, data, sizeof(PayloadType));
			    typedCallback(payload);
		    }
		);
	}

	SignalResult unsubscribe(SignalSubscriptionId id);

	SignalResult post(SignalEventId eventId);

	template <
	    typename Event,
	    typename std::enable_if_t<
	        signal_detail::IsSignalEventType<Event>::value &&
	            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Event>>, SignalEventId>,
	        int> = 0>
	SignalResult post(Event event) {
		return post(signal_detail::eventToId(event));
	}

	template <typename Event, typename Payload>
	SignalResult post(Event event, const Payload &payload) {
		using PayloadType = std::remove_cv_t<std::remove_reference_t<Payload>>;
		static_assert(signal_detail::IsSignalEventType<Event>::value, "Invalid Signal event ID type");
		static_assert(
		    std::is_trivially_copyable_v<PayloadType>,
		    "Signal payloads must be trivially copyable"
		);
		return postRaw(signal_detail::eventToId(event), sizeof(PayloadType), &payload, 0, true);
	}

	SignalResult postWithTimeout(SignalEventId eventId, uint32_t timeoutMs);

	template <
	    typename Event,
	    typename std::enable_if_t<
	        signal_detail::IsSignalEventType<Event>::value &&
	            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Event>>, SignalEventId>,
	        int> = 0>
	SignalResult postWithTimeout(Event event, uint32_t timeoutMs) {
		return postWithTimeout(signal_detail::eventToId(event), timeoutMs);
	}

	template <typename Event, typename Payload>
	SignalResult postWithTimeout(Event event, const Payload &payload, uint32_t timeoutMs) {
		using PayloadType = std::remove_cv_t<std::remove_reference_t<Payload>>;
		static_assert(signal_detail::IsSignalEventType<Event>::value, "Invalid Signal event ID type");
		static_assert(
		    std::is_trivially_copyable_v<PayloadType>,
		    "Signal payloads must be trivially copyable"
		);
		return postRaw(
		    signal_detail::eventToId(event),
		    sizeof(PayloadType),
		    &payload,
		    timeoutMs,
		    false
		);
	}

	SignalResult waitFor(SignalEventId eventId, uint32_t timeoutMs);

	template <
	    typename Event,
	    typename std::enable_if_t<
	        signal_detail::IsSignalEventType<Event>::value &&
	            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Event>>, SignalEventId>,
	        int> = 0>
	SignalResult waitFor(Event event, uint32_t timeoutMs) {
		return waitFor(signal_detail::eventToId(event), timeoutMs);
	}

	template <typename Event, typename Payload>
	SignalResult waitFor(Event event, Payload &out, uint32_t timeoutMs) {
		using PayloadType = std::remove_cv_t<std::remove_reference_t<Payload>>;
		static_assert(signal_detail::IsSignalEventType<Event>::value, "Invalid Signal event ID type");
		static_assert(
		    std::is_trivially_copyable_v<PayloadType>,
		    "Signal payloads must be trivially copyable"
		);
		return waitForRaw(signal_detail::eventToId(event), sizeof(PayloadType), &out, timeoutMs);
	}

	SignalDiag getDiagnostics();

	const char *statusToString(SignalStatus status) const;

  private:
	using SignalRawCallback = std::function<void(const void *, size_t)>;

	SignalSubResult subscribeRaw(
	    SignalEventId eventId,
	    size_t payloadSize,
	    SignalRawCallback callback
	);
	SignalResult postRaw(
	    SignalEventId eventId,
	    size_t payloadSize,
	    const void *payload,
	    uint32_t timeoutMs,
	    bool useDefaultTimeout
	);
	SignalResult waitForRaw(
	    SignalEventId eventId,
	    size_t payloadSize,
	    void *payloadOut,
	    uint32_t timeoutMs
	);

	std::unique_ptr<SignalImpl> _impl;
};
