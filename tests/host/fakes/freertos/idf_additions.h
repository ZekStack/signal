#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

BaseType_t xTaskCreatePinnedToCoreWithCaps(
    TaskFunction_t entry,
    const char *name,
    configSTACK_DEPTH_TYPE stackDepth,
    void *arg,
    UBaseType_t priority,
    TaskHandle_t *handle,
    BaseType_t coreId,
    UBaseType_t memoryCaps
);
void vTaskDeleteWithCaps(TaskHandle_t handle);

#ifdef __cplusplus
}
#endif
