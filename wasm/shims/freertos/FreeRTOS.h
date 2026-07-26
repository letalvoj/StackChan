#ifndef INC_FREERTOS_H
#define INC_FREERTOS_H

#include <stdint.h>
#include <stddef.h>

typedef void* TaskHandle_t;
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0xFFFFFFFFUL

#endif // INC_FREERTOS_H
