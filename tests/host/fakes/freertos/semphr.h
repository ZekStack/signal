#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *controlBlock);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *controlBlock);
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *controlBlock);
SemaphoreHandle_t xSemaphoreCreateCountingStatic(
    UBaseType_t maxCount,
    UBaseType_t initialCount,
    StaticSemaphore_t *controlBlock
);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t handle, TickType_t timeout);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t handle);
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle);
BaseType_t xSemaphoreTakeFromISR(
    SemaphoreHandle_t handle,
    BaseType_t *higherPriorityTaskWoken
);
BaseType_t xSemaphoreGiveFromISR(
    SemaphoreHandle_t handle,
    BaseType_t *higherPriorityTaskWoken
);
void vSemaphoreDelete(SemaphoreHandle_t handle);

#ifdef __cplusplus
}
#endif
