#include "sht3x_driver.h"
#include "driver/i2c.h"

#define SHT3X_ADDR 0x44
#define I2C_MASTER_NUM I2C_NUM_0

static bool initialized = false;

esp_err_t sht3x_init(int sda_pin, int scl_pin) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (ret != ESP_OK) return ret;
    ret = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    if (ret == ESP_OK) initialized = true;
    return ret;
}

esp_err_t sht3x_start_measurement(void) {
    if (!initialized) return ESP_FAIL;
    uint8_t cmd[2] = {0x2C, 0x06}; // Single shot, high repeatability
    return i2c_master_write_to_device(I2C_MASTER_NUM, SHT3X_ADDR, cmd, 2, pdMS_TO_TICKS(100));
}

esp_err_t sht3x_read_result(float *temp, float *hum) {
    if (!initialized) return ESP_FAIL;
    uint8_t data[6];
    esp_err_t ret = i2c_master_read_from_device(I2C_MASTER_NUM, SHT3X_ADDR, data, 6, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) return ret;

    uint16_t raw_t = (data[0] << 8) | data[1];
    uint16_t raw_h = (data[3] << 8) | data[4];
    *temp = -45.0f + 175.0f * raw_t / 65535.0f;
    *hum = 100.0f * raw_h / 65535.0f;
    return ESP_OK;
}

esp_err_t sht3x_deinit(void) {
    if (initialized) {
        i2c_driver_delete(I2C_MASTER_NUM);
        initialized = false;
    }
    return ESP_OK;
}