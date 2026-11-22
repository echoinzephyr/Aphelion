#pragma once

#include "terminal.h"
#include "lvgl.h"

typedef struct {
    lv_obj_t *body;
    lv_obj_t *canvas;
    lv_obj_t *labels[8];
    uint8_t *pixels;
    int height;
} oled_layout_t;

typedef struct {
    terminal_view_t view;
    bool setup;
    bool synced;
    bool working;
    struct tm local;
    environment_t environment;
    sound_t sound;
    char ap[16];
    char key[13];
} oled_content_t;

void oled_layout_init(oled_layout_t *layout, lv_obj_t *screen, uint8_t *pixels, int height);
void oled_layout_update(oled_layout_t *layout, const oled_content_t *content, unsigned shift);
