#ifndef WASM_LOG_H
#define WASM_LOG_H

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <hal/hal.h>

/**
 * WASM_LOG_FIRST_N(N, fmt, ...)
 * Log the first N invocations at this call site, then print a single
 * "throttled" message and go permanently silent.
 */
#define WASM_LOG_FIRST_N(N, fmt, ...) do { \
    static int _wlog_cnt = 0; \
    if (_wlog_cnt < (N)) { \
        _wlog_cnt++; \
        printf(fmt, ##__VA_ARGS__); \
        if (_wlog_cnt == (N)) { \
            printf("[WASM_LOG] (Throttled: suppressing further repetitions of this log)\n"); \
        } \
    } \
} while (0)

/**
 * WASM_LOG_EVERY_N(N, fmt, ...)
 * Log every Nth invocation at this call site (modulo counter).
 */
#define WASM_LOG_EVERY_N(N, fmt, ...) do { \
    static int _wlog_cnt = 0; \
    if (_wlog_cnt++ % (N) == 0) printf(fmt, ##__VA_ARGS__); \
} while (0)

/**
 * WASM_LOG_THROTTLE(interval_ms, tag, fmt, ...)
 * Time-based throttle: log at most once per interval_ms. When suppressed
 * frames exist, append the skip count to the output.
 */
#define WASM_LOG_THROTTLE(interval_ms, tag, fmt, ...) do { \
    static uint32_t _wlog_last_time = 0; \
    static uint32_t _wlog_skip_count = 0; \
    uint32_t _wlog_now = GetHAL().millis(); \
    if (_wlog_last_time == 0 || (_wlog_now - _wlog_last_time) >= (interval_ms)) { \
        if (_wlog_skip_count > 0) { \
            printf("[%s] " fmt " (skipped %u logs in last %ums)\n", \
                   tag, ##__VA_ARGS__, _wlog_skip_count, _wlog_now - _wlog_last_time); \
        } else { \
            printf("[%s] " fmt "\n", tag, ##__VA_ARGS__); \
        } \
        _wlog_last_time = _wlog_now; \
        _wlog_skip_count = 0; \
    } else { \
        _wlog_skip_count++; \
    } \
} while (0)

#endif // WASM_LOG_H
