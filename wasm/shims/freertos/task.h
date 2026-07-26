#ifndef TASK_H
#define TASK_H

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define taskSCHEDULER_RUNNING 1
static inline int xTaskGetSchedulerState(void) { return taskSCHEDULER_RUNNING; }
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)0x1000; }
static inline void vTaskDelay(TickType_t xTicksToDelay) { (void)xTicksToDelay; }

#ifdef __cplusplus
}
#endif

#endif // TASK_H
