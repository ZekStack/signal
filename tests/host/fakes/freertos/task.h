#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

TaskHandle_t xTaskCreateStatic(
    TaskFunction_t entry,
    const char *name,
    configSTACK_DEPTH_TYPE stackDepth,
    void *arg,
    UBaseType_t priority,
    StackType_t *stack,
    StaticTask_t *controlBlock
);
TaskHandle_t xTaskCreateStaticPinnedToCore(
    TaskFunction_t entry,
    const char *name,
    configSTACK_DEPTH_TYPE stackDepth,
    void *arg,
    UBaseType_t priority,
    StackType_t *stack,
    StaticTask_t *controlBlock,
    BaseType_t coreId
);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskNotifyGive(TaskHandle_t handle);
uint32_t ulTaskNotifyTake(BaseType_t clearCountOnExit, TickType_t timeout);
void vTaskDelay(TickType_t ticks);
void vTaskSuspend(TaskHandle_t handle);
void vTaskDelete(TaskHandle_t handle);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t handle);

#ifdef __cplusplus
}
#endif
