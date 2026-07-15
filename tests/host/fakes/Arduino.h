#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

inline uint32_t millis() {
	static const auto started = std::chrono::steady_clock::now();
	const auto elapsed = std::chrono::steady_clock::now() - started;
	return static_cast<uint32_t>(
	    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
	);
}

inline void delay(uint32_t milliseconds) {
	std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}
