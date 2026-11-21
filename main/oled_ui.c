#include "oled.h"
#include "network.h"
#include "sensors.h"
#include "sdkconfig.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#if CONFIG_APHELION_OLED_SSD1306_32
enum { OLED_HEIGHT = 32 };
#else
enum { OLED_HEIGHT = 64 };
#endif

enum { ROWS = OLED_HEIGHT / 16 };
static const char *TAG = "oled";
static portMUX_TYPE view_lock = portMUX_INITIALIZER_UNLOCKED;
static bool display_ready;
static terminal_view_t selected_view = TERMINAL_OVERVIEW;

bool oled_is_ready(void)
{
    portENTER_CRITICAL(&view_lock);
    bool ready = display_ready;
    portEXIT_CRITICAL(&view_lock);
    return ready;
}

static void set_display_ready(bool ready)
{
    portENTER_CRITICAL(&view_lock);
    display_ready = ready;
    portEXIT_CRITICAL(&view_lock);
}

terminal_view_t oled_selected_view(void)
{
    portENTER_CRITICAL(&view_lock);
    terminal_view_t view = selected_view;
    portEXIT_CRITICAL(&view_lock);
    return view;
}

terminal_view_t oled_press(void)
{
    portENTER_CRITICAL(&view_lock);
    terminal_view_t next = terminal_next_view(selected_view);
    selected_view = next;
    portEXIT_CRITICAL(&view_lock);
    return next;
}

typedef struct {
    i2c_master_dev_handle_t device;
    esp_err_t flush_status;
} oled_ui_t;

static uint32_t elapsed_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    static uint8_t frame[OLED_FRAME_BYTES];
    oled_ui_t *ui = lv_display_get_user_data(display);
    // Rasterize antialiased glyphs before thresholding them for the 1-bit OLED.
    terminal_oled_pack(pixels, OLED_HEIGHT, frame);
    ui->flush_status = oled_write(ui->device, frame, OLED_HEIGHT);
    lv_display_flush_ready(display);
}

static void set_label(lv_obj_t *label, const char *text, const lv_font_t *preferred)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        const lv_font_t *font = preferred;
        if (lv_text_get_width(text, strlen(text), font, 0) > OLED_WIDTH - 8) {
            font = &lv_font_unscii_8;
        }
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_pad_top(label, (16 - (int)font->line_height) / 2, 0);
        lv_label_set_text(label, text);
    }
}

void oled_task(void *arg)
{
    static _Alignas(4) uint8_t draw_buffer[OLED_WIDTH * OLED_HEIGHT];
    i2c_master_bus_handle_t bus = arg;
    oled_ui_t ui = {0};
    lv_init();
    lv_tick_set_cb(elapsed_ms);
    lv_display_t *display = lv_display_create(OLED_WIDTH, OLED_HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_L8);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_user_data(display, &ui);
    lv_display_set_flush_cb(display, flush);

    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_remove_style_all(screen);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *labels[ROWS];
    for (unsigned row = 0; row < ROWS; ++row) {
        lv_obj_t *label = lv_label_create(screen);
        lv_obj_remove_style_all(label);
        lv_obj_set_size(label, OLED_WIDTH, 16);
        lv_obj_set_pos(label, 0, row * 16);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_pad_left(label, 4, 0);
        lv_obj_set_style_pad_right(label, 4, 0);
        lv_obj_set_style_text_color(label, row == 0 ? lv_color_black() : lv_color_white(), 0);
        if (row == 0) {
            lv_obj_set_style_bg_color(label, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
        }
        lv_label_set_text(label, "");
        labels[row] = label;
    }
    bool missing_reported = false;
    for (;;) {
        if (!ui.device) {
            esp_err_t err = oled_open(bus, &ui.device);
            if (err != ESP_OK) {
                if (!missing_reported) {
                    ESP_LOGW(TAG, "OLED unavailable on 0x3c/0x3d: %s", esp_err_to_name(err));
                    missing_reported = true;
                }
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            missing_reported = false;
            lv_obj_invalidate(screen);
        }

        environment_t env;
        sound_t audio;
        sensor_snapshot(&env, &audio);
        network_info_t net = network_snapshot();
        terminal_page_t page;
        terminal_format(net.ip[0] ? oled_selected_view() : TERMINAL_OVERVIEW,
                        &env, &audio, net.ip, net.ap, net.key, &page);
        unsigned start = ROWS == 2 && (elapsed_ms() / 4000) % 2 ? 2 : 0;
        for (unsigned row = 0; row < ROWS; ++row) {
            set_label(labels[row], page.line[start + row],
                      row == 0 ? &lv_font_montserrat_14 : &lv_font_montserrat_12);
        }
        lv_timer_handler();
        if (ui.flush_status != ESP_OK) {
            ESP_LOGW(TAG, "OLED write failed: %s", esp_err_to_name(ui.flush_status));
            i2c_master_bus_rm_device(ui.device);
            ui.device = NULL;
            set_display_ready(false);
            ui.flush_status = ESP_OK;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        set_display_ready(true);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
