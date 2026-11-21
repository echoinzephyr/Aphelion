#pragma once

#include "measurements.h"

enum { OLED_WIDTH = 128, OLED_MAX_HEIGHT = 64, OLED_FRAME_BYTES = OLED_WIDTH * OLED_MAX_HEIGHT / 8 };

typedef enum {
    TERMINAL_OVERVIEW,
    TERMINAL_ENVIRONMENT,
    TERMINAL_SOUND,
    TERMINAL_VIEW_COUNT,
} terminal_view_t;

typedef struct {
    char line[4][19];
} terminal_page_t;

const char *sample_status_label(sample_status_t status);
const char *terminal_view_name(terminal_view_t view);
terminal_view_t terminal_next_view(terminal_view_t view);
void terminal_format(terminal_view_t view, const environment_t *env, const sound_t *sound, const char *ip,
                     const char *ap, const char *key, terminal_page_t *page);
// Convert LVGL's row-major luminance pixels to the OLED's vertical page bytes.
void terminal_oled_pack(const uint8_t *pixels, unsigned height, uint8_t frame[OLED_FRAME_BYTES]);
