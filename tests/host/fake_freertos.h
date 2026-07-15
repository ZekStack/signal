#pragma once

#include <cstddef>
#include <cstdint>

namespace fake_freertos {
void reset();
void setPsramBytes(size_t bytes);
void failNextTaskCreates(uint32_t count);
void failNextCapsTaskCreates(uint32_t count);
void setTaskCreateDelayMs(uint32_t milliseconds);
uint32_t taskCreateCount();
uint32_t capsTaskCreateCount();
}
