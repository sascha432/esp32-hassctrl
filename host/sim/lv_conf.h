#ifndef LV_CONF_H
#define LV_CONF_H

/* LVGL 8.4 settings of the Windows version of the UI (docs/architecture.md section 1).
 *
 * The firmware sets LVGL through Kconfig (sdkconfig.main, boards/<board>/sdkconfig.defaults, the
 * defaults of managed_components/lvgl__lvgl/Kconfig). This file holds the same values, read from
 * build/wt32_sc01/main/sdkconfig; everything not listed here keeps LVGL's own default, as in the
 * firmware. tools/sim.py compares the two and warns when a value differs.
 *
 * Deliberate differences, all invisible in the picture:
 *   LV_COLOR_16_SWAP 0   the firmware swaps the bytes for its SPI panel, a PC does not
 *   LV_ASSERT_HANDLER    stops with a message instead of looping forever
 *   LV_LOG_PRINTF        the sim prints LVGL's log itself (main.cpp)
 *   LV_ATTRIBUTE_FAST_MEM  IRAM placement means nothing on a PC
 */

/*====================  Color  ====================*/
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0
#define LV_COLOR_SCREEN_TRANSP 0
#define LV_COLOR_MIX_ROUND_OFS 128
#define LV_COLOR_CHROMA_KEY lv_color_hex(0x00FF00)

/*====================  Memory  ====================*/
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_BUF_MAX_NUM 16

/*====================  HAL  ====================*/
#define LV_DISP_DEF_REFR_PERIOD 10
#define LV_INDEV_DEF_READ_PERIOD 30
#define LV_TICK_CUSTOM 0
#define LV_DPI_DEF 130

/*====================  Drawing  ====================*/
#define LV_DRAW_COMPLEX 1
#define LV_SHADOW_CACHE_SIZE 0
#define LV_CIRCLE_CACHE_SIZE 4
#define LV_LAYER_SIMPLE_BUF_SIZE (24 * 1024)
#define LV_IMG_CACHE_DEF_SIZE 0
#define LV_GRADIENT_MAX_STOPS 2
#define LV_GRAD_CACHE_DEF_SIZE 0
#define LV_DITHER_GRADIENT 0
#define LV_DISP_ROT_MAX_BUF (10 * 1024)

/*====================  Log  ====================*/
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 0
#define LV_LOG_TRACE_MEM 1
#define LV_LOG_TRACE_TIMER 1
#define LV_LOG_TRACE_INDEV 1
#define LV_LOG_TRACE_DISP_REFR 1
#define LV_LOG_TRACE_EVENT 1
#define LV_LOG_TRACE_OBJ_CREATE 1
#define LV_LOG_TRACE_LAYOUT 1
#define LV_LOG_TRACE_ANIM 1

/*====================  Asserts  ====================*/
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0
#define LV_ASSERT_HANDLER_INCLUDE "sim_assert.h"
#define LV_ASSERT_HANDLER                                                                                              \
    do {                                                                                                               \
        fprintf(stderr, "LVGL assert at %s:%d\n", __FILE__, __LINE__);                                                 \
        fflush(stderr);                                                                                                \
        abort();                                                                                                       \
    } while (0);

/*====================  Others  ====================*/
/* the firmware shows LVGL's FPS/CPU label in the bottom right corner; the numbers mean nothing on
 * a PC, the label is kept because it covers the same pixels */
#define LV_USE_PERF_MONITOR 1
#define LV_PERF_MONITOR_ALIGN LV_ALIGN_BOTTOM_RIGHT
#define LV_USE_REFR_DEBUG 0
#define LV_SPRINTF_CUSTOM 0
#define LV_USE_USER_DATA 1
#define LV_ENABLE_GC 0
#define LV_BIG_ENDIAN_SYSTEM 0
#define LV_ATTRIBUTE_MEM_ALIGN_SIZE 1
#define LV_USE_LARGE_COORD 0

/*====================  Fonts  ====================*/
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
/* the larger fonts of the JC4880P443C (boards/jc4880p443c/sdkconfig.defaults) */
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_34 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_USE_FONT_PLACEHOLDER 1
#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_SUBPX 0

/*====================  Text  ====================*/
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_TXT_COLOR_CMD "#"
#define LV_USE_BIDI 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

/*====================  Widgets  ====================*/
#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_BTN 1
#define LV_USE_BTNMATRIX 1
#define LV_USE_CANVAS 1
#define LV_USE_CHECKBOX 1
#define LV_USE_DROPDOWN 1
#define LV_USE_IMG 1
#define LV_USE_LABEL 1
#define LV_LABEL_TEXT_SELECTION 1
#define LV_LABEL_LONG_TXT_HINT 1
#define LV_USE_LINE 1
#define LV_USE_ROLLER 1
#define LV_ROLLER_INF_PAGES 7
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1
#define LV_USE_TEXTAREA 1
#define LV_TEXTAREA_DEF_PWD_SHOW_TIME 1500
#define LV_USE_TABLE 1

/*====================  Extra widgets  ====================*/
#define LV_USE_ANIMIMG 1
#define LV_USE_CALENDAR 1
#define LV_CALENDAR_WEEK_STARTS_MONDAY 0
#define LV_USE_CALENDAR_HEADER_ARROW 1
#define LV_USE_CALENDAR_HEADER_DROPDOWN 1
#define LV_USE_CHART 1
#define LV_USE_COLORWHEEL 1
#define LV_USE_IMGBTN 1
#define LV_USE_KEYBOARD 1
#define LV_USE_LED 1
#define LV_USE_LIST 1
#define LV_USE_MENU 1
#define LV_USE_METER 1
#define LV_USE_MSGBOX 1
#define LV_USE_SPAN 1
#define LV_SPAN_SNIPPET_STACK_SIZE 64
#define LV_USE_SPINBOX 1
#define LV_USE_SPINNER 1
#define LV_USE_TABVIEW 1
#define LV_USE_TILEVIEW 1
#define LV_USE_WIN 1

/*====================  Themes and layouts  ====================*/
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 0
#define LV_THEME_DEFAULT_GROW 1
#define LV_THEME_DEFAULT_TRANSITION_TIME 80
#define LV_USE_THEME_BASIC 1
#define LV_USE_THEME_MONO 0
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/*====================  Libraries and 3rd party  ====================*/
#define LV_USE_QRCODE 1
#define LV_USE_SNAPSHOT 1
#define LV_BUILD_EXAMPLES 1

#endif /*LV_CONF_H*/
