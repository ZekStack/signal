#pragma once

#include <Arduino.h>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <type_traits>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace zek::signal {

using SignalEventId = uint32_t;
using SignalSubscriptionId = uint32_t;

struct SignalImpl;
class Signal;

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
	const char *message = "error";

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
	uint32_t processedEventCount = 0;
	uint32_t callbackInvokeCount = 0;
	uint32_t dispatchedCount = 0;
	uint32_t droppedCount = 0;
	uint32_t rejectedCount = 0;
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
using SignalRawCallback = void (*)(void *context);
using SignalRawPayloadCallback = void (*)(void *context, const void *payload, size_t payloadSize);

class SignalSubscriptionHandle {
  public:
	SignalSubscriptionHandle() = default;
	SignalSubscriptionHandle(Signal *bus, SignalSubscriptionId id);
	~SignalSubscriptionHandle();

	SignalSubscriptionHandle(const SignalSubscriptionHandle &) = delete;
	SignalSubscriptionHandle &operator=(const SignalSubscriptionHandle &) = delete;

	SignalSubscriptionHandle(SignalSubscriptionHandle &&other) noexcept;
	SignalSubscriptionHandle &operator=(SignalSubscriptionHandle &&other) noexcept;

	explicit operator bool() const {
		return _bus != nullptr && _id != 0;
	}

	SignalSubscriptionId id() const {
		return _id;
	}

	SignalResult unsubscribe();
	SignalSubscriptionId release();

  private:
	Signal *_bus = nullptr;
	SignalSubscriptionId _id = 0;
};

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
	SignalSubscriptionHandle subscribeHandle(SignalEventId eventId, SignalCallback callback);

	template <
	    typename Event,
	    typename std::enable_if_t<
	        signal_detail::IsSignalEventType<Event>::value &&
	            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Event>>, SignalEventId>,
	        int> = 0>
	SignalSubResult subscribe(Event event, SignalCallback callback) {
		return subscribe(signal_detail::eventToId(event), callback);
	}

	template <
	    typename Event,
	    typename std::enable_if_t<
	        signal_detail::IsSignalEventType<Event>::value &&
	            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Event>>, SignalEventId>,
	        int> = 0>
	SignalSubscriptionHandle subscribeHandle(Event event, SignalCallback callback) {
		return subscribeHandle(signal_detail::eventToId(event), callback);
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

		return subscribeFunction(
		    signal_detail::eventToId(event),
		    sizeof(PayloadType),
		    [typedCallback](const void *data, size_t size) {
			    if (data == nullptr || size != sizeof(PayloadType)) {
				    return;
			    }
			    std::array<std::byte, sizeof(PayloadType)> bytes{};
			    memcpy(bytes.data(), data, sizeof(PayloadType));
			    const PayloadType payload = std::bit_cast<PayloadType>(bytes);
			    typedCallback(payload);
		    }
		);
	}

	template <typename Payload, typename Event, typename Callback>
	SignalSubscriptionHandle subscribeHandle(Event event, Callback callback) {
		SignalSubResult result = subscribe<Payload>(event, callback);
		if (!result) {
			return SignalSubscriptionHandle();
		}
		return SignalSubscriptionHandle(this, result.id);
	}

	SignalSubResult subscribeRaw(
	    SignalEventId eventId,
	    SignalRawCallback callback,
	    void *context = nullptr
	);
	SignalSubResult subscribeRaw(
	    SignalEventId eventId,
	    size_t payloadSize,
	    SignalRawPayloadCallback callback,
	    void *context = nullptr
	);
	SignalSubscriptionHandle subscribeRawHandle(
	    SignalEventId eventId,
	    SignalRawCallback callback,
	    void *context = nullptr
	);
	SignalSubscriptionHandle subscribeRawHandle(
	    SignalEventId eventId,
	    size_t payloadSize,
	    SignalRawPayloadCallback callback,
	    void *context = nullptr
	);

	template <
	    typename Event,
	    typename std::enable_if_t<
	        signal_detail::IsSignalEventType<Event>::value &&
	            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Event>>, SignalEventId>,
	        int> = 0>
	SignalSubResult subscribeRaw(Event event, SignalRawCallback callback, void *context = nullptr) {
		return subscribeRaw(signal_detail::eventToId(event), callback, context);
	}

	template <
	    typename Event,
	    typename std::enable_if_t<
	        signal_detail::IsSignalEventType<Event>::value &&
	            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Event>>, SignalEventId>,
	        int> = 0>
	SignalSubResult subscribeRaw(
	    Event event,
	    size_t payloadSize,
	    SignalRawPayloadCallback callback,
	    void *context = nullptr
	) {
		return subscribeRaw(signal_detail::eventToId(event), payloadSize, callback, context);
	}

	template <
	    typename Event,
	    typename std::enable_if_t<
	        signal_detail::IsSignalEventType<Event>::value &&
	            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Event>>, SignalEventId>,
	        int> = 0>
	SignalSubscriptionHandle subscribeRawHandle(
	    Event event,
	    SignalRawCallback callback,
	    void *context = nullptr
	) {
		return subscribeRawHandle(signal_detail::eventToId(event), callback, context);
	}

	template <
	    typename Event,
	    typename std::enable_if_t<
	        signal_detail::IsSignalEventType<Event>::value &&
	            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Event>>, SignalEventId>,
	        int> = 0>
	SignalSubscriptionHandle subscribeRawHandle(
	    Event event,
	    size_t payloadSize,
	    SignalRawPayloadCallback callback,
	    void *context = nullptr
	) {
		return subscribeRawHandle(signal_detail::eventToId(event), payloadSize, callback, context);
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
	using SignalFunctionCallback = std::function<void(const void *, size_t)>;

	SignalSubResult subscribeFunction(
	    SignalEventId eventId,
	    size_t payloadSize,
	    SignalFunctionCallback callback
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

} // namespace zek::signal

#ifndef ZEK_SIGNAL_DISABLE_GLOBAL_ALIASES
using Signal = zek::signal::Signal;
using SignalCallback = zek::signal::SignalCallback;
using SignalConfig = zek::signal::SignalConfig;
using SignalDiag = zek::signal::SignalDiag;
using SignalEventId = zek::signal::SignalEventId;
using SignalOverflowPolicy = zek::signal::SignalOverflowPolicy;
using SignalRawCallback = zek::signal::SignalRawCallback;
using SignalRawPayloadCallback = zek::signal::SignalRawPayloadCallback;
using SignalResult = zek::signal::SignalResult;
using SignalStackType = zek::signal::SignalStackType;
using SignalStatus = zek::signal::SignalStatus;
using SignalSubResult = zek::signal::SignalSubResult;
using SignalSubscriptionHandle = zek::signal::SignalSubscriptionHandle;
using SignalSubscriptionId = zek::signal::SignalSubscriptionId;
#endif
