#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t maxCount, UBaseType_t initialCount);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t handle, TickType_t timeout);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t handle);
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle);
void vSemaphoreDelete(SemaphoreHandle_t handle);

#ifdef __cplusplus
}
#endif
