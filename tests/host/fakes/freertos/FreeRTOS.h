#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int BaseType_t;
typedef uint32_t UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef uint32_t configSTACK_DEPTH_TYPE;

struct FakeTask;
struct FakeSemaphore;

typedef struct FakeTask *TaskHandle_t;
typedef struct FakeSemaphore *SemaphoreHandle_t;
typedef void (*TaskFunction_t)(void *);

typedef struct StaticTask_t {
	uint32_t marker;
} StaticTask_t;

typedef struct StaticSemaphore_t {
	uint32_t marker;
} StaticSemaphore_t;

#define pdFALSE 0
#define pdTRUE 1
#define pdFAIL 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define tskNO_AFFINITY (-1)
#define tskIDLE_PRIORITY 0
#define configSUPPORT_STATIC_ALLOCATION 1
#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 1
#define INCLUDE_vTaskDelete 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#ifdef __cplusplus
}
#endif
