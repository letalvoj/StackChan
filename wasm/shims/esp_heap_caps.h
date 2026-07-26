#ifndef ESP_HEAP_CAPS_SHIM_H
#define ESP_HEAP_CAPS_SHIM_H

#include <cstdlib>
#include <cstdint>

#define MALLOC_CAP_SPIRAM (1 << 0)
#define MALLOC_CAP_8BIT   (1 << 1)

static inline void* heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}

static inline void heap_caps_free(void* ptr) {
    free(ptr);
}

#endif // ESP_HEAP_CAPS_SHIM_H
