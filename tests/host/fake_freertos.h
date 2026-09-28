#pragma once

#include <cstdint>

namespace fake_freertos {
void reset();
void failNextTaskCreates(uint32_t count);
void setTaskCreateDelayMs(uint32_t milliseconds);
uint32_t taskCreateCount();
}
