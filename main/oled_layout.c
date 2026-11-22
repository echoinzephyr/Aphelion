#include "oled_layout.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(aphelion_oled_labels_12);

static void block(oled_layout_t *layout, int x, int y, int width, int height)
{
    for (int row = y; row < y + height; row++) {
        memset(&layout->pixels[row * OLED_WIDTH + x], 255, width);
    }
}

static void dotted_line(oled_layout_t *layout, int x, int y, int width, int height)
{
    for (int row = y; row < y + height; row += 2) {
        for (int column = x; column < x + width; column += 2) {
            block(layout, column, row, 1, 1);
        }
    }
}

static void label(oled_layout_t *layout, unsigned i, int x, int y, const char *text)
{
    lv_obj_t *obj = layout->labels[i];
    lv_obj_set_pos(obj, x, y);
    if (strcmp(lv_label_get_text(obj), text) != 0) lv_label_set_text(obj, text);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

enum { TOP = 1, UPPER_RIGHT = 2, LOWER_RIGHT = 4, BOTTOM = 8,
       LOWER_LEFT = 16, UPPER_LEFT = 32, MID = 64 };

typedef struct {
    int width;
    int height;
    int stroke;
    int scale;
} digit_font_t;

static const digit_font_t small_digits = {7, 12, 2, 1};
static const digit_font_t large_digits = {13, 24, 3, 1};
static const digit_font_t watch_digits = {13, 24, 3, 2};

static int digit_width(char ch, const digit_font_t *font)
{
    return ch == ':' || ch == '.' ? font->stroke : font->width;
}

static int digits_width(const char *text, const digit_font_t *font)
{
    int width = 0;
    for (const char *ch = text; *ch; ch++) width += digit_width(*ch, font) + 2;
    return (width - 2) * font->scale;
}

static void digits(oled_layout_t *layout, int x, int y, const char *text, const digit_font_t *font)
{
    static const uint8_t lit[] = {
        TOP | UPPER_RIGHT | LOWER_RIGHT | BOTTOM | LOWER_LEFT | UPPER_LEFT,
        UPPER_RIGHT | LOWER_RIGHT,
        TOP | UPPER_RIGHT | MID | LOWER_LEFT | BOTTOM,
        TOP | UPPER_RIGHT | MID | LOWER_RIGHT | BOTTOM,
        UPPER_LEFT | MID | UPPER_RIGHT | LOWER_RIGHT,
        TOP | UPPER_LEFT | MID | LOWER_RIGHT | BOTTOM,
        TOP | UPPER_LEFT | MID | LOWER_LEFT | LOWER_RIGHT | BOTTOM,
        TOP | UPPER_RIGHT | LOWER_RIGHT,
        TOP | UPPER_RIGHT | LOWER_RIGHT | BOTTOM | LOWER_LEFT | UPPER_LEFT | MID,
        TOP | UPPER_RIGHT | LOWER_RIGHT | BOTTOM | UPPER_LEFT | MID,
    };
    const int s = font->scale;
    const int w = font->width * s, h = font->height * s, t = font->stroke * s;
    const int m = (font->height / 2 - 1) * s;
    for (; *text; text++) {
        if (*text == ':' || *text == '.') {
            block(layout, x, y + (*text == ':' ? h / 4 : h - t), t, t);
            if (*text == ':') block(layout, x, y + h * 3 / 4 - t, t, t);
        } else {
            const uint8_t segments = *text >= '0' && *text <= '9' ? lit[*text - '0'] : MID;
            if (segments & TOP) block(layout, x, y, w, t);
            if (segments & MID) block(layout, x, y + m, w, t);
            if (segments & BOTTOM) block(layout, x, y + h - t, w, t);
            if (segments & UPPER_LEFT) block(layout, x, y, t, m + t);
            if (segments & UPPER_RIGHT) block(layout, x + w - t, y, t, m + t);
            if (segments & LOWER_LEFT) block(layout, x, y + m, t, h - m);
            if (segments & LOWER_RIGHT) block(layout, x + w - t, y + m, t, h - m);
        }
        x += (digit_width(*text, font) + 2) * s;
    }
}

static void pages(oled_layout_t *layout, terminal_view_t page)
{
    if (layout->height == 32) {
        for (unsigned i = 0; i < TERMINAL_VIEW_COUNT; i++) {
            int height = i == page ? 6 : 2;
            block(layout, 123, 3 + i * 7 + (6 - height) / 2, 2, height);
        }
        return;
    }
    for (unsigned i = 0; i < TERMINAL_VIEW_COUNT; i++) {
        int width = i == page ? 6 : 2;
        block(layout, 47 + i * 9 + (6 - width) / 2, 59, width, 2);
    }
}

static void wifi_icon(oled_layout_t *layout, int x, int y)
{
    block(layout, x, y + 1, 8, 1);
    block(layout, x + 1, y + 3, 6, 1);
    block(layout, x + 2, y + 5, 4, 1);
    block(layout, x + 3, y + 7, 2, 1);
}

static void work_icon(oled_layout_t *layout, int x, int y)
{
    block(layout, x, y + 4, 3, 2);
    block(layout, x + 3, y + 1, 2, 5);
    block(layout, x + 5, y + 1, 3, 2);
}

static void watch(oled_layout_t *layout, const oled_content_t *content)
{
    static const char *const weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
    char time[6] = "--:--", date[32] = "未校时", seconds[3];
    const bool compact = layout->height == 32;
    if (content->synced) {
        strftime(time, sizeof(time), "%H:%M", &content->local);
        if (compact) {
            strftime(date, sizeof(date), "%m/%d", &content->local);
        } else {
            snprintf(date, sizeof(date), "%d月%d日 周%s", content->local.tm_mon + 1,
                     content->local.tm_mday, weekdays[content->local.tm_wday]);
            strftime(seconds, sizeof(seconds), "%S", &content->local);
            label(layout, 1, 95, 49, seconds);
        }
    }

    if (compact) {
        digits(layout, 3, 3, time, &large_digits);
        label(layout, 0, 72, 9, date);
        pages(layout, TERMINAL_CLOCK);
    } else {
        digits(layout, (126 - digits_width(time, &watch_digits)) / 2, 0, time, &watch_digits);
        label(layout, 0, 2, 49, date);
        if (content->working) work_icon(layout, 116, 50);
        else wifi_icon(layout, 116, 49);
    }
}

static void setup_screen(oled_layout_t *layout, const oled_content_t *content)
{
    if (layout->height == 32) {
        // The hotspot name and password are both needed to connect from a phone.
        label(layout, 0, 2, 1, content->ap);
        label(layout, 1, 2, 16, content->key);
        return;
    }
    label(layout, 0, 2, 0, "Wi-Fi 配网");
    dotted_line(layout, 2, 13, 122, 1);
    label(layout, 1, 2, 16, content->ap);
    label(layout, 2, 2, 31, "密码");
    label(layout, 4, 28, 31, content->key);
    label(layout, 3, 2, 47, "192.168.4.1");
}

static void format_reading(char out[12], bool valid, float value)
{
    if (valid && isfinite(value)) snprintf(out, 12, "%.1f", value);
    else strcpy(out, "-");
}

static void measurements(oled_layout_t *layout, const oled_content_t *content)
{
    const environment_t *env = &content->environment;
    const sound_t *sound = &content->sound;
    const bool env_ok = env->status == SAMPLE_OK;
    const bool sound_ok = sound->status == SAMPLE_OK || sound->status == SAMPLE_CLIPPING;
    char temperature[12], humidity[12], loudness[12], level[12];
    format_reading(temperature, env_ok, env->celsius);
    format_reading(humidity, env_ok, env->humidity);
    format_reading(loudness, sound_ok, sound->spl);
    format_reading(level, sound_ok, sound->dbfs);

    const char *const headings[] = {"温度", "湿度", "声音"};
    const char *const units[] = {"°C", "%RH", "dB SPL"};
    const char *const readings[] = {temperature, humidity, loudness};
    const unsigned view = content->view;
    if (layout->height == 32) {
        label(layout, 0, 2, 1, headings[view]);
        label(layout, 1, 2, 15, units[view]);
        digits(layout, 50, 3, readings[view], &large_digits);
        pages(layout, content->view);
        return;
    }

    const char *const titles[] = {"传感器", "温湿度", "声音"};
    label(layout, 0, 2, 0, titles[view]);
    if (content->synced) {
        char time[6];
        strftime(time, sizeof(time), "%H:%M", &content->local);
        label(layout, 1, 74, 0, time);
    }
    if (content->working) work_icon(layout, 116, 2);
    else wifi_icon(layout, 116, 1);
    dotted_line(layout, 0, 13, 126, 1);
    pages(layout, content->view);

    if (view == TERMINAL_OVERVIEW) {
        for (unsigned i = 0; i < 3; i++) {
            const int x = i * 42 + 2;
            label(layout, i + 2, x, 16, headings[i]);
            digits(layout, x, 31, readings[i], &small_digits);
            label(layout, i + 5, x, 44, i == 2 ? "dB" : units[i]);
            if (i) dotted_line(layout, x - 3, 16, 1, 38);
        }
    } else {
        const char *left = view == TERMINAL_ENVIRONMENT ? temperature : loudness;
        const char *right = view == TERMINAL_ENVIRONMENT ? humidity : level;
        label(layout, 2, 2, 43, view == TERMINAL_ENVIRONMENT ? "°C" : "dB SPL");
        label(layout, 3, 65, 43, view == TERMINAL_ENVIRONMENT ? "%RH" : "dBFS");
        const digit_font_t *right_font = digits_width(right, &large_digits) <= 60 ? &large_digits : &small_digits;
        const digit_font_t *left_font = digits_width(left, &large_digits) <= 60 ? &large_digits : &small_digits;
        digits(layout, 2, 41 - left_font->height, left, left_font);
        digits(layout, 65, 41 - right_font->height, right, right_font);
        dotted_line(layout, 63, 16, 1, 38);
    }
}

void oled_layout_init(oled_layout_t *layout, lv_obj_t *screen, uint8_t *pixels, int height)
{
    layout->height = height;
    layout->pixels = pixels;
    layout->body = lv_obj_create(screen);
    lv_obj_remove_style_all(layout->body);
    lv_obj_set_size(layout->body, OLED_WIDTH, height);
    lv_obj_clear_flag(layout->body, LV_OBJ_FLAG_SCROLLABLE);
    layout->canvas = lv_canvas_create(layout->body);
    lv_canvas_set_buffer(layout->canvas, pixels, OLED_WIDTH, height, LV_COLOR_FORMAT_L8);
    lv_obj_set_style_bg_opa(layout->canvas, LV_OPA_TRANSP, 0);
    for (unsigned i = 0; i < sizeof(layout->labels) / sizeof(layout->labels[0]); i++) {
        lv_obj_t *obj = lv_label_create(layout->body);
        lv_obj_remove_style_all(obj);
        lv_obj_set_style_text_font(obj, &aphelion_oled_labels_12, 0);
        lv_obj_set_style_text_color(obj, lv_color_white(), 0);
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        layout->labels[i] = obj;
    }
}

void oled_layout_update(oled_layout_t *layout, const oled_content_t *content, unsigned shift)
{
    memset(layout->pixels, 0, OLED_WIDTH * layout->height);
    lv_obj_set_pos(layout->body, shift & 1, (shift >> 1) & 1);
    for (unsigned i = 0; i < sizeof(layout->labels) / sizeof(layout->labels[0]); i++) {
        lv_obj_add_flag(layout->labels[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (content->setup) setup_screen(layout, content);
    else if (content->view == TERMINAL_CLOCK) watch(layout, content);
    else measurements(layout, content);
    lv_obj_invalidate(layout->canvas);
}
