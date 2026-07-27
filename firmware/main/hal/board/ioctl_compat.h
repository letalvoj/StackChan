/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Deliberately NO include guard: this header is a preprocessor action, not a
 * declaration set, and must stay usable at more than one include boundary
 * within the same translation unit.
 *
 * esp_video's <linux/ioctl.h> and lwip's <lwip/sockets.h> both define
 * _IO / _IOR / _IOW (/_IOWR) with *incompatible* encodings:
 *
 *   lwip      _IO(x,y)     -> ((long)(IOC_VOID|((x)<<8)|(y)))        BSD
 *   esp_video _IO(type,nr) -> _IOC(_IOC_NONE,(type),(nr),0)          Linux
 *
 * Neither guards against the other -- lwip's block is gated on
 * `#if !defined(FIONREAD) || !defined(FIONBIO)`, which esp_video never
 * defines. So whichever header lands second silently redefines the macros,
 * and any ioctl constant expanded after that point is built with the wrong
 * encoding.
 *
 * Include this immediately before the header whose encoding the translation
 * unit actually needs, to drop the previous definitions cleanly.
 *
 * CAUTION: only safe in a TU that does not use ioctl constants from the family
 * being discarded (FIONREAD / FIONBIO / SIOC* for lwip, VIDIOC_* for V4L2).
 * Those constants expand _IO*() at their *use* site, not at their definition
 * site, so discarding the wrong family silently corrupts their values.
 */

#undef _IO
#undef _IOR
#undef _IOW
#undef _IOWR
