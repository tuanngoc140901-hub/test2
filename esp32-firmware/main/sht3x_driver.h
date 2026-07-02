#pragma once
#include "esp_err.h"

esp_err_t sht3x_init(int sda_pin, int scl_pin);
esp_err_t sht3x_read_raw(float *temp, float *hum);
esp_err_t sht3x_deinit(void);
