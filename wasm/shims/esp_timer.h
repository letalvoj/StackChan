#ifndef ESP_TIMER_H
#define ESP_TIMER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void* esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void* arg);
typedef enum {
    ESP_TIMER_TASK,
} esp_timer_dispatch_t;
typedef struct {
    esp_timer_cb_t callback;
    void* arg;
    esp_timer_dispatch_t dispatch_method;
    const char* name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;
static inline int64_t esp_timer_get_time(void) { return 0; }
static inline int esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* out_handle) { if (out_handle) *out_handle = (esp_timer_handle_t)1; return 0; }
static inline int esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us) { return 0; }
static inline int esp_timer_stop(esp_timer_handle_t timer) { return 0; }
static inline int esp_timer_delete(esp_timer_handle_t timer) { return 0; }
#ifdef __cplusplus
}
#endif
#endif // ESP_TIMER_H
