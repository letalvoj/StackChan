#ifndef ESP_PM_H
#define ESP_PM_H
#include "esp_err.h"
typedef void* esp_pm_lock_handle_t;
#define ESP_PM_CPU_FREQ_MAX 0
static inline esp_err_t esp_pm_lock_create(int type, int arg, const char* name, esp_pm_lock_handle_t* out_handle) { if (out_handle) *out_handle = (esp_pm_lock_handle_t)1; return ESP_OK; }
static inline esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t handle) { return ESP_OK; }
static inline esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t handle) { return ESP_OK; }
static inline esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t handle) { return ESP_OK; }
#endif // ESP_PM_H
