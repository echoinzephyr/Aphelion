#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SAMPLE_WAITING, SAMPLE_OK, SAMPLE_IO_ERROR, SAMPLE_TIMEOUT, SAMPLE_BAD_CRC,
    SAMPLE_ALL_ZERO, SAMPLE_NO_AC, SAMPLE_CLIPPING,
} sample_status_t;

typedef struct {
    sample_status_t status;
    float celsius;
    float humidity;
    int64_t last_good_us;
} environment_t;

typedef struct {
    sample_status_t status;
    float dbfs;
    float spl;
    int64_t sampled_us;
} sound_t;

typedef struct {
    uint32_t count;
    int64_t sum;
    int64_t sum_squares;
    bool clipping;
} sound_window_t;

bool sht40_decode(const uint8_t frame[6], environment_t *reading);
// INMP441 delivers a signed 24-bit sample in the high bits of each I2S slot.
void sound_accumulate(sound_window_t *window, const int32_t *slots, size_t count);
sound_t sound_measure(const sound_window_t *window);
