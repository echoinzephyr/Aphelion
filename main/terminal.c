#include "terminal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

const char *sample_status_label(sample_status_t status)
{
    switch (status) {
    case SAMPLE_OK:       return "正常";
    case SAMPLE_IO_ERROR: return "通信异常";
    case SAMPLE_TIMEOUT:  return "读取超时";
    case SAMPLE_BAD_CRC:  return "校验失败";
    case SAMPLE_ALL_ZERO: return "采样全零";
    case SAMPLE_NO_AC:    return "仅直流";
    case SAMPLE_CLIPPING: return "削波";
    default:              return "等待采样";
    }
}

const char *terminal_view_name(terminal_view_t view)
{
    static const char *const names[] = {"overview", "environment", "sound", "clock"};
    return names[view];
}

terminal_view_t terminal_next_view(terminal_view_t view)
{
    return (view + 1) % TERMINAL_VIEW_COUNT;
}

void terminal_format(terminal_view_t view, const environment_t *env, const sound_t *sound, const char *ip,
                     const char *ap, const char *key, terminal_page_t *page)
{
    memset(page, 0, sizeof(*page));
    const size_t cap = sizeof(page->line[0]);
    const bool env_ok = env->status == SAMPLE_OK;
    const bool mic_ok = isfinite(sound->dbfs) &&
                        (sound->status == SAMPLE_OK || sound->status == SAMPLE_CLIPPING);

    if (!ip[0] && ap[0]) {
        snprintf(page->line[0], cap, "%s", ap);
        snprintf(page->line[1], cap, "PW %s", key);
        snprintf(page->line[2], cap, "192.168.4.1");
    } else if (view == TERMINAL_ENVIRONMENT) {
        snprintf(page->line[0], cap, "SHT40");
        if (env_ok) {
            snprintf(page->line[1], cap, "Temp  %.1f C", env->celsius);
            snprintf(page->line[2], cap, "Humidity %.1f%%", env->humidity);
        } else {
            snprintf(page->line[1], cap, "Temp   -- C");
            snprintf(page->line[2], cap, "Humidity --%%");
        }
        snprintf(page->line[3], cap, "%s", env_ok ? "Sensor online" : "No reading");
        return;
    } else if (view == TERMINAL_SOUND) {
        snprintf(page->line[0], cap, "INMP441");
        if (mic_ok) {
            snprintf(page->line[1], cap, "%.1f dBFS", sound->dbfs);
            snprintf(page->line[2], cap, "~%.1f dB SPL", sound->spl);
        } else {
            snprintf(page->line[1], cap, "-- dBFS");
            snprintf(page->line[2], cap, "-- dB SPL");
        }
        snprintf(page->line[3], cap, "%s", sound->status == SAMPLE_CLIPPING ? "Clipping" : "SPL estimated");
        return;
    } else {
        snprintf(page->line[0], cap, "%s", ip[0] ? ip : "Aphelion");
        if (env_ok) {
            snprintf(page->line[1], cap, "%.1f C  %.1f%%RH", env->celsius, env->humidity);
        } else {
            snprintf(page->line[1], cap, "-- C     --%%RH");
        }
        if (mic_ok) {
            snprintf(page->line[2], cap, "%.1f dBFS%s", sound->dbfs,
                     sound->status == SAMPLE_CLIPPING ? " !" : "");
        } else {
            snprintf(page->line[2], cap, "-- dBFS");
        }
    }

    if (mic_ok && isfinite(sound->spl)) {
        snprintf(page->line[3], cap, "~%.1f dB SPL", sound->spl);
    } else {
        snprintf(page->line[3], cap, "-- dB SPL");
    }
}

void terminal_format_clock(const struct tm *local, const char *address, terminal_page_t *page)
{
    memset(page, 0, sizeof(*page));
    const size_t cap = sizeof(page->line[0]);
    if (local) {
        strftime(page->line[0], cap, "%H:%M:%S", local);
        strftime(page->line[1], cap, "%Y-%m-%d", local);
        strftime(page->line[2], cap, "%a  UTC+8", local);
    } else {
        snprintf(page->line[0], cap, "--:--:--");
        snprintf(page->line[1], cap, "Waiting for NTP");
        snprintf(page->line[2], cap, "UTC+8");
    }
    snprintf(page->line[3], cap, "%s", address);
}
