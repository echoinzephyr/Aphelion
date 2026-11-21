#include "measurements.h"

#include <math.h>

static uint8_t sht_crc(const uint8_t *word)
{
    uint8_t crc = 0xff;
    for (int i = 0; i < 2; ++i) {
        crc ^= word[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
        }
    }
    return crc;
}

bool sht40_decode(const uint8_t frame[6], environment_t *reading)
{
    if (sht_crc(frame) != frame[2] || sht_crc(frame + 3) != frame[5]) {
        return false;
    }
    unsigned temp = (frame[0] << 8) | frame[1];
    unsigned rh = (frame[3] << 8) | frame[4];
    reading->celsius = -45.0f + 175.0f * temp / 65535.0f;
    reading->humidity = fminf(100.0f, fmaxf(0.0f, -6.0f + 125.0f * rh / 65535.0f));
    return true;
}

void sound_accumulate(sound_window_t *window, const int32_t *slots, size_t count)
{
    // A one-second window at 16 kHz fits in a signed 64-bit sum of squares.
    for (size_t i = 0; i < count; ++i) {
        int32_t sample = slots[i] >> 8;
        window->sum += sample;
        window->sum_squares += (int64_t)sample * sample;
        if (sample == 8388607 || sample == -8388608) {
            window->clipping = true;
        }
    }
    window->count += count;
}

sound_t sound_measure(const sound_window_t *window)
{
    sound_t reading = { .status = SAMPLE_ALL_ZERO };
    if (window->count == 0 || window->sum_squares == 0) {
        return reading;
    }
    double mean = (double)window->sum / window->count;
    double power = (double)window->sum_squares / window->count - mean * mean;
    if (power <= 0.0) {
        reading.status = window->clipping ? SAMPLE_CLIPPING : SAMPLE_NO_AC;
        reading.dbfs = reading.spl = -INFINITY;
        return reading;
    }

    // The manufacturer's 0 dBFS reference is the RMS of a full-scale sine.
    reading.dbfs = 10.0 * log10(2.0 * power / (8388608.0 * 8388608.0));
    reading.spl = reading.dbfs + 120.0f; // nominal -26 dBFS at 94 dB SPL
    reading.status = window->clipping ? SAMPLE_CLIPPING : SAMPLE_OK;
    return reading;
}
