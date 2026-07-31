/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstddef>

namespace stackchan {

/**
 * @brief In-memory ring buffer of the most recent ESP_LOG output.
 *
 * Exists because the USB CDC console is a genuinely bad debugging channel on this
 * device, in three separate ways this project has been bitten by:
 *
 *   1. It dies with the thing you are trying to debug. A panic kills TinyUSB before
 *      the backtrace flushes, so the most valuable log lines are exactly the ones
 *      you never see (see DEBUGGING.md 4).
 *   2. It only exists while a cable does. The whole point of the Tailscale work is
 *      to debug the robot with no cable attached -- and that is precisely when the
 *      console is unavailable.
 *   3. Capturing it is a race. A host-side `cat` has to already be attached and
 *      alive before the interesting thing happens, and it dies silently on reboot.
 *
 * A ring buffer in RAM has none of those problems: it is already recording before
 * anything goes wrong, it survives a reboot no better than the console does, but
 * unlike the console it can be READ BACK over the network afterwards, from
 * whichever transport is currently up (USB or tailnet -- the WebSocket server binds
 * INADDR_ANY, so GET /debug/logs works over both).
 *
 * Deliberately PSRAM-backed and modest: internal RAM is the scarce resource on this
 * board (the whole reason microlink's task stacks had to move), so this must not
 * compete for it.
 */
class LogRing {
public:
    /** @brief Install the log hook. Idempotent; safe to call before anything logs. */
    static void Install();

    /**
     * @brief Copy the most recent bytes into `out`, oldest first.
     * @return Number of bytes written (never more than `out_size - 1`; NUL-terminated).
     *
     * Safe to call from another task while logging continues -- takes the same lock
     * the writer does.
     */
    static size_t Snapshot(char* out, size_t out_size);

    /** @brief Bytes currently buffered. */
    static size_t Size();
};

}  // namespace stackchan
