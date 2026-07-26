#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>
#include <stdio.h>

#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

#define LV_USE_OS               LV_OS_NONE
#define LV_USE_LOG              1
#define LV_LOG_LEVEL            LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF           1

#define LV_USE_FONT_COMPRESSED  1
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_16   1
#define LV_FONT_MONTSERRAT_20   1
#define LV_FONT_MONTSERRAT_24   1
#define LV_FONT_MONTSERRAT_30   1

#define LV_USE_QRCODE           1

#define LV_USE_SYSMON           0
#define LV_USE_PERF_MONITOR     0
#define LV_USE_MEM_MONITOR      0

#define LV_USE_DRAW_SW          1
#define LV_DRAW_SW_SUPPORT_RGB565 1

#endif /* LV_CONF_H */
