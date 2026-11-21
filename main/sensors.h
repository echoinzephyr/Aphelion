#pragma once

#include "measurements.h"
#include "esp_err.h"

// Called by the two independent acquisition tasks with a complete snapshot.
void publish_environment(const environment_t *reading);
void publish_sound(const sound_t *reading);

void sensor_snapshot(environment_t *env, sound_t *sound);

// In the toolbox profile, start dashboard peripherals only when an OLED answers.
esp_err_t aphelion_start_peripherals(bool optional);

void environment_task(void *arg);
void sound_task(void *arg);
