#include "oled.h"
#include "oled_layout.h"
#include "device_clock.h"
#include "network.h"
#include "sensors.h"
#include "sdkconfig.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_APHELION_TOOLBOX
#include "toolbox/toolbox.h"
#endif

#if CONFIG_APHELION_OLED_SSD1306_32
enum { OLED_HEIGHT = 32 };
#else
enum { OLED_HEIGHT = 64 };
#endif

static const char *TAG = "oled";
static portMUX_TYPE view_lock = portMUX_INITIALIZER_UNLOCKED;
static bool display_ready;
static terminal_view_t selected_view = TERMINAL_CLOCK;

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
    // Pack the LVGL canvas and monochrome glyphs into the OLED controller pages.
    terminal_oled_pack(pixels, OLED_HEIGHT, frame);
    ui->flush_status = oled_write(ui->device, frame, OLED_HEIGHT);
    lv_display_flush_ready(display);
}

void oled_task(void *arg)
{
    static _Alignas(4) uint8_t draw_buffer[OLED_WIDTH * OLED_HEIGHT];
    static _Alignas(4) uint8_t layout_buffer[OLED_WIDTH * OLED_HEIGHT];
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

    oled_layout_t layout;
    oled_layout_init(&layout, screen, layout_buffer, OLED_HEIGHT);
    oled_content_t last = {0};
    unsigned last_shift = 0;
    bool redraw = true;
    bool missing_reported = false;
    uint32_t next_update = 0;
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
            redraw = true;
        }

        network_info_t net = network_snapshot();
        oled_content_t content = {
            .view = oled_selected_view(),
            .setup = !net.ip[0],
        };
        content.synced = !content.setup && device_clock_read(&content.local);
        if (content.setup) {
            memcpy(content.ap, net.ap, sizeof(content.ap));
            memcpy(content.key, net.key, sizeof(content.key));
        } else if (content.view != TERMINAL_CLOCK) {
            sensor_snapshot(&content.environment, &content.sound);
        }
#if CONFIG_APHELION_TOOLBOX
        content.working = toolbox_signal_status().pwm;
#endif
        const uint32_t now = elapsed_ms();
        const unsigned shift = (now / 60000) % 4;
        const bool page_changed = content.view != last.view || content.setup != last.setup;
        const bool update = content.synced != last.synced || content.working != last.working ||
                            content.local.tm_sec != last.local.tm_sec ||
                            content.local.tm_min != last.local.tm_min ||
                            content.local.tm_hour != last.local.tm_hour ||
                            content.local.tm_yday != last.local.tm_yday ||
                            content.environment.last_good_us != last.environment.last_good_us ||
                            content.environment.status != last.environment.status ||
                            content.sound.sampled_us != last.sound.sampled_us ||
                            content.sound.status != last.sound.status ||
                            strcmp(content.ap, last.ap) != 0 || strcmp(content.key, last.key) != 0;
        if (redraw || page_changed || shift != last_shift ||
            (update && (int32_t)(now - next_update) >= 0)) {
            oled_layout_update(&layout, &content, shift);
            last = content;
            last_shift = shift;
            redraw = false;
            next_update = now + 250;
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
