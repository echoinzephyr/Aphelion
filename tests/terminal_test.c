// Host test: cc -std=c11 -Imain tests/terminal_test.c main/{measurements,terminal,terminal_draw}.c -lm -o /tmp/aphelion-test
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "terminal.h"

static void test_sht40(void)
{
    uint8_t frame[] = {0x80, 0, 0xa2, 0x80, 0, 0xa2};
    environment_t env = {0};
    assert(sht40_decode(frame, &env));
    assert(fabs(env.celsius - 42.5013) < 0.001);
    assert(fabs(env.humidity - 56.5010) < 0.001);
    frame[5] ^= 1;
    assert(!sht40_decode(frame, &env));
    frame[5] ^= 1;
    frame[2] ^= 1;
    assert(!sht40_decode(frame, &env));
    const uint8_t low[] = {0, 0, 0x81, 0, 0, 0x81};
    const uint8_t high[] = {255, 255, 0xac, 255, 255, 0xac};
    assert(sht40_decode(low, &env) && env.humidity == 0 && env.celsius == -45);
    assert(sht40_decode(high, &env) && env.humidity == 100 && env.celsius == 130);
}

static sound_t tone(double amplitude, int dc)
{
    sound_window_t window = {0};
    for (int block = 0; block < 1000; ++block) {
        int32_t slots[16]; // one period at 1 kHz, 16 kHz sample rate
        for (int i = 0; i < 16; ++i) {
            double v = amplitude * sin(2 * 3.141592653589793 * i / 16);
            slots[i] = ((int32_t)lround(v * 8388608) + dc) * 256 + 0x55;
        }
        sound_accumulate(&window, slots, 16);
    }
    return sound_measure(&window);
}

static void test_audio(void)
{
    double amplitude = pow(10, -26.0 / 20);
    sound_t ref = tone(amplitude, 0);
    sound_t half = tone(amplitude / 2, 50000);
    assert(ref.status == SAMPLE_OK && fabs(ref.dbfs + 26) < 0.001 && fabs(ref.spl - 94) < 0.001);
    assert(fabs(ref.dbfs - half.dbfs - 6.0206) < 0.001);
    assert(fabs(ref.spl - half.spl - 6.0206) < 0.001);
    assert(tone(0, 0).status == SAMPLE_ALL_ZERO);
    assert(tone(0, -100).status == SAMPLE_NO_AC);

    sound_window_t window = {0};
    int32_t rails[] = {INT32_MAX, INT32_MIN};
    sound_accumulate(&window, rails, 2);
    sound_t clipped = sound_measure(&window);
    assert(clipped.status == SAMPLE_CLIPPING && isfinite(clipped.dbfs));
}

static void test_pages(void)
{
    environment_t env = {.status = SAMPLE_OK, .celsius = -45, .humidity = 100};
    sound_t sound = {.status = SAMPLE_OK, .dbfs = -26, .spl = 94};
    terminal_page_t page;

    const char *ip = "192.168.31.207";
    terminal_view_t view = TERMINAL_OVERVIEW;
    terminal_format(view, &env, &sound, ip, "", "", &page);
    assert(strcmp(page.line[0], "192.168.31.207") == 0);
    assert(strcmp(page.line[1], "-45.0 C  100.0%RH") == 0);
    assert(strcmp(page.line[2], "-26.0 dBFS") == 0);
    assert(strcmp(page.line[3], "~94.0 dB SPL") == 0);

    view = terminal_next_view(view);
    assert(strcmp(terminal_view_name(view), "environment") == 0);
    terminal_format(view, &env, &sound, ip, "", "", &page);
    assert(strcmp(page.line[0], "SHT40") == 0);
    assert(strcmp(page.line[2], "Humidity 100.0%") == 0);

    view = terminal_next_view(view);
    assert(strcmp(terminal_view_name(view), "sound") == 0);
    terminal_format(view, &env, &sound, ip, "", "", &page);
    assert(strcmp(page.line[0], "INMP441") == 0);
    assert(strcmp(page.line[1], "-26.0 dBFS") == 0);
    assert(strcmp(page.line[2], "~94.0 dB SPL") == 0);
    assert(terminal_next_view(view) == TERMINAL_OVERVIEW);

    terminal_format(view, &env, &sound, "", "Aphelion-4D05", "ABCDEFGH2345", &page);
    assert(strcmp(page.line[0], "Aphelion-4D05") == 0);
    assert(strcmp(page.line[1], "PW ABCDEFGH2345") == 0);
    assert(strcmp(page.line[2], "192.168.4.1") == 0);

    env.status = SAMPLE_BAD_CRC;
    sound.status = SAMPLE_ALL_ZERO;
    terminal_format(TERMINAL_OVERVIEW, &env, &sound, "", "", "", &page);
    assert(strcmp(page.line[0], "Aphelion") == 0);
    assert(strcmp(page.line[1], "-- C     --%RH") == 0);
    assert(strcmp(page.line[2], "-- dBFS") == 0);
    assert(strcmp(page.line[3], "-- dB SPL") == 0);
    assert(strcmp(sample_status_label(env.status), "校验失败") == 0);
}

static void test_oled_layout(void)
{
    uint8_t horizontal[OLED_WIDTH * OLED_MAX_HEIGHT] = {0};
    horizontal[0] = horizontal[7] = 255;
    horizontal[7 * OLED_WIDTH] = 132;
    horizontal[8 * OLED_WIDTH + 9] = 255;
    horizontal[sizeof(horizontal) - 1] = 255;
    horizontal[1] = 100;
    uint8_t vertical[OLED_FRAME_BYTES];
    terminal_oled_pack(horizontal, 64, vertical);
    assert(vertical[0] == 0x81);
    assert(vertical[7] == 0x01);
    assert(vertical[OLED_WIDTH + 9] == 0x01);
    assert(vertical[OLED_FRAME_BYTES - 1] == 0x80);
    assert(vertical[1] == 0);

    memset(vertical, 0xff, sizeof(vertical));
    terminal_oled_pack(horizontal, 32, vertical);
    assert(vertical[OLED_WIDTH + 9] == 0x01);
    assert(vertical[OLED_FRAME_BYTES / 2 - 1] == 0);
    assert(vertical[OLED_FRAME_BYTES / 2] == 0xff);
}

int main(void)
{
    test_sht40();
    test_audio();
    test_pages();
    test_oled_layout();
    puts("PASS: SHT40, INMP441, OLED page layout and pixel packing");
    return 0;
}
