#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALLOC_CAP_SPIRAM ((uint32_t)0x01)
#define MALLOC_CAP_8BIT ((uint32_t)0x02)

size_t heap_caps_get_total_size(uint32_t caps);

#ifdef __cplusplus
}
#endif
