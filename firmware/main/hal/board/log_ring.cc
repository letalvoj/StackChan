/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "log_ring.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_private/cache_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace stackchan {

namespace {

// 16 KB of scrollback. Enough to hold a full boot plus a minute or so of steady
// state, which is the window that has actually mattered when debugging this device.
// PSRAM, not internal: internal RAM is the scarce resource here and this buffer has
// no reason to compete for it.
constexpr size_t kRingSize = 16 * 1024;

char* g_ring = nullptr;
size_t g_head = 0;        // next write position
size_t g_filled = 0;      // bytes valid (< kRingSize until it wraps once)
SemaphoreHandle_t g_lock = nullptr;
vprintf_like_t g_chain = nullptr;   // the previous sink (the UART/USB console)

void RingWrite(const char* data, size_t len)
{
    if (g_ring == nullptr || len == 0) {
        return;
    }
    // A single write larger than the ring can only ever leave its own tail behind.
    if (len >= kRingSize) {
        data += (len - kRingSize);
        len = kRingSize;
    }
    const size_t first = (kRingSize - g_head < len) ? (kRingSize - g_head) : len;
    memcpy(g_ring + g_head, data, first);
    if (first < len) {
        memcpy(g_ring, data + first, len - first);
    }
    g_head = (g_head + len) % kRingSize;
    g_filled = (g_filled + len > kRingSize) ? kRingSize : (g_filled + len);
}

int LogHook(const char* fmt, va_list args)
{
    // vsnprintf twice would double the cost of every log line, and va_list is
    // single-use, so copy it for the pass-through sink.
    va_list args_for_chain;
    va_copy(args_for_chain, args);

    char line[256];
    const int n = vsnprintf(line, sizeof(line), fmt, args);

    // THE RING LIVES IN PSRAM, AND PSRAM IS UNREACHABLE WHILE THE CACHE IS OFF.
    //
    // Flash and PSRAM share the SPI bus and cache on this chip, so any flash write
    // disables the cache PSRAM depends on. A log line emitted inside that window --
    // and ml_peer_nvs.c emits several while saving peers, which is exactly when this
    // fires -- would touch g_ring and reset the chip. Silently: printing a panic
    // needs the cache too, so the failure produces no backtrace at all, just a
    // reboot. That is precisely the signature this caused on hardware (repeated
    // silent reboots immediately after "ml_wg_mgr: CMM endpoint", with log lines
    // visibly truncated mid-write).
    //
    // Same hazard class as microlink's task stacks; the difference is a stack cannot
    // be conditionally skipped and this can. The line still reaches the real console
    // below, so nothing is lost but the scrollback copy.
    const bool cache_ok = spi_flash_cache_enabled();

    if (n > 0 && cache_ok && g_lock != nullptr) {
        // Never block a logging call: a log line is not worth a priority inversion,
        // and losing one line from the ring is strictly better than stalling the
        // task that emitted it. Same reasoning as dropping a mic frame elsewhere in
        // this codebase rather than blocking the audio path.
        if (xSemaphoreTake(g_lock, 0) == pdTRUE) {
            const size_t len = (n < static_cast<int>(sizeof(line))) ? static_cast<size_t>(n)
                                                                   : sizeof(line) - 1;
            RingWrite(line, len);
            xSemaphoreGive(g_lock);
        }
    }

    // Still write to the original console. This ADDS a channel, it does not replace
    // one -- the CDC console remains the fastest way to watch a live boot with a
    // cable attached.
    int rc = n;
    if (g_chain != nullptr) {
        rc = g_chain(fmt, args_for_chain);
    }
    va_end(args_for_chain);
    return rc;
}

}  // namespace

void LogRing::Install()
{
    if (g_ring != nullptr) {
        return;   // idempotent
    }
    g_ring = static_cast<char*>(heap_caps_malloc(kRingSize, MALLOC_CAP_SPIRAM));
    if (g_ring == nullptr) {
        // Not fatal, and deliberately not retried: a device that cannot spare 16 KB
        // of PSRAM has bigger problems, and this is a debugging aid, not a feature.
        ESP_LOGW("LogRing", "could not allocate %u bytes of PSRAM; scrollback disabled",
                 static_cast<unsigned>(kRingSize));
        return;
    }
    g_lock = xSemaphoreCreateMutex();
    if (g_lock == nullptr) {
        heap_caps_free(g_ring);
        g_ring = nullptr;
        return;
    }
    g_chain = esp_log_set_vprintf(LogHook);
    ESP_LOGI("LogRing", "scrollback enabled (%u bytes, PSRAM); GET /debug/logs",
             static_cast<unsigned>(kRingSize));
}

size_t LogRing::Snapshot(char* out, size_t out_size)
{
    if (out == nullptr || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (g_ring == nullptr || g_lock == nullptr) {
        return 0;
    }
    // A reader MAY wait -- unlike the writer above, this is a human-initiated HTTP
    // request and blocking briefly is fine.
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return 0;
    }

    const size_t avail = g_filled;
    const size_t want = (avail < out_size - 1) ? avail : (out_size - 1);
    // Oldest-first: start `want` bytes back from the head, wrapping.
    const size_t start = (g_head + kRingSize - want) % kRingSize;
    const size_t first = (kRingSize - start < want) ? (kRingSize - start) : want;
    memcpy(out, g_ring + start, first);
    if (first < want) {
        memcpy(out + first, g_ring, want - first);
    }
    out[want] = '\0';

    xSemaphoreGive(g_lock);
    return want;
}

size_t LogRing::Size()
{
    return g_filled;
}

}  // namespace stackchan
