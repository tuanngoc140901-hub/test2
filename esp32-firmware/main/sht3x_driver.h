#pragma once
#include "esp_err.h"

esp_err_t sht3x_init(int sda_pin, int scl_pin);
esp_err_t sht3x_start_measurement(void);      // Gửi lệnh đo
esp_err_t sht3x_read_result(float *temp, float *hum); // Đọc kết quả
esp_err_t sht3x_deinit(void);