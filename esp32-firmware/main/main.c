#include <stdio.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "sht3x_driver.h"
#include "filter_lib.h"
#include "wifi_mqtt.h"

static const char *TAG = "main";

// ==================== Bộ lọc trung vị ====================
static median_filter_t temp_filter;
static median_filter_t humid_filter;

// ==================== ADC oneshot ====================
static adc_oneshot_unit_handle_t adc1_handle;
#define NTC_ADC_CHANNEL   ADC_CHANNEL_7   // GPIO35

// ==================== I2C cho SHT3x ====================
#define I2C_MASTER_SDA_IO   21
#define I2C_MASTER_SCL_IO   22

// ==================== Điều khiển nguồn cảm biến (MOSFET P-channel) ====================
#define SENSOR_PWR_GPIO    GPIO_NUM_26    // LOW = bật
static bool sensor_powered = true;

// ==================== Peltier (MOSFET FR120N trên D18) ====================
#define PELTIER_PWR_GPIO   GPIO_NUM_18   // HIGH = bật Peltier

// Ngưỡng nhiệt độ (đo từ NTC)
#define TEMP_LOW_THRESHOLD   20.0f   // < 20°C -> bật sưởi
#define TEMP_HIGH_THRESHOLD  30.0f   // > 30°C -> bật làm mát
#define TEMP_HYSTERESIS      2.0f    // Trễ 2°C

// Chế độ Peltier: "off", "cool", "heat", "auto"
static char peltier_mode[8] = "off";   // mặc định tắt
static bool peltier_on = false;

// ==================== Forward declarations ====================
static void sensor_power_on(void);
static void sensor_power_off(void);
static void i2c_recovery(void);
static void adc_init(void);
static int read_adc_raw(adc_channel_t channel);
static float read_ntc_temp(void);
static void process_command(const char *json);

// ==================== Nguồn cảm biến ====================
static void sensor_power_on(void) {
    if (sensor_powered) return;
    gpio_set_level(SENSOR_PWR_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    sht3x_init(I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
    sensor_powered = true;
    ESP_LOGI(TAG, "Sensor power ON");
}

static void sensor_power_off(void) {
    if (!sensor_powered) return;
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << I2C_MASTER_SDA_IO) | (1ULL << I2C_MASTER_SCL_IO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    sht3x_deinit();
    gpio_set_level(SENSOR_PWR_GPIO, 1);
    sensor_powered = false;
    ESP_LOGI(TAG, "Sensor power OFF");
}

// ==================== Phục hồi I2C ====================
static void i2c_recovery(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << I2C_MASTER_SDA_IO) | (1ULL << I2C_MASTER_SCL_IO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    for (int i = 0; i < 9; i++) {
        gpio_set_level(I2C_MASTER_SCL_IO, 0);
        esp_rom_delay_us(5);
        gpio_set_level(I2C_MASTER_SCL_IO, 1);
        esp_rom_delay_us(5);
    }
    sht3x_deinit();
    vTaskDelay(pdMS_TO_TICKS(10));
    if (sensor_powered) sht3x_init(I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
}

// ==================== ADC ====================
static void adc_init(void) {
    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc1_handle));
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, NTC_ADC_CHANNEL, &chan_cfg));
}

static int read_adc_raw(adc_channel_t channel) {
    int raw;
    adc_oneshot_read(adc1_handle, channel, &raw);
    return raw;
}

static float read_ntc_temp(void) {
    int raw = read_adc_raw(NTC_ADC_CHANNEL);
    float voltage = raw * 3.3f / 4095.0f;
    float r_fixed = 10000.0f;
    float r_ntc = r_fixed * (3.3f - voltage) / voltage;
    float T0 = 298.15f;
    float R0 = 10000.0f;
    float B = 3950.0f;
    float steinhart = logf(r_ntc / R0) / B + 1.0f / T0;
    return 1.0f / steinhart - 273.15f;
}

// ==================== Hàm điều khiển Peltier ====================
static void update_peltier(void) {
    float temp = read_ntc_temp();
    bool should_on = false;

    if (strcmp(peltier_mode, "cool") == 0) {
        if (temp > TEMP_HIGH_THRESHOLD + TEMP_HYSTERESIS) {
            should_on = true;
        } else if (temp < TEMP_HIGH_THRESHOLD - TEMP_HYSTERESIS) {
            should_on = false;
        } else {
            should_on = peltier_on;
        }
    } else if (strcmp(peltier_mode, "heat") == 0) {
        if (temp < TEMP_LOW_THRESHOLD - TEMP_HYSTERESIS) {
            should_on = true;
        } else if (temp > TEMP_LOW_THRESHOLD + TEMP_HYSTERESIS) {
            should_on = false;
        } else {
            should_on = peltier_on;
        }
    } else if (strcmp(peltier_mode, "auto") == 0) {
        if (temp > TEMP_HIGH_THRESHOLD) {
            should_on = true;
        } else if (temp < TEMP_LOW_THRESHOLD) {
            should_on = true;
        } else {
            should_on = false;
        }
    } else { // "off"
        should_on = false;
    }

    if (should_on != peltier_on) {
        peltier_on = should_on;
        gpio_set_level(PELTIER_PWR_GPIO, peltier_on ? 1 : 0);
        ESP_LOGI(TAG, "Peltier %s (temp=%.1f, mode=%s)", peltier_on ? "ON" : "OFF", temp, peltier_mode);
    }
}

// ==================== Task điều khiển Peltier ====================
void peltier_control_task(void *arg) {
    esp_task_wdt_add(NULL);
    gpio_set_direction(PELTIER_PWR_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(PELTIER_PWR_GPIO, 0);

    while (1) {
        update_peltier();
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ==================== Task cảm biến chính ====================
void sensor_task(void *arg) {
    esp_task_wdt_add(NULL);
    float raw_t, raw_h;

    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000);

    while (1) {
        if (sensor_powered) {
            esp_err_t ret = sht3x_read_raw(&raw_t, &raw_h);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "I2C error, attempt recovery");
                i2c_recovery();
                raw_t = 25.0f;
                raw_h = 60.0f;
            }
        } else {
            raw_t = 25.0f;
            raw_h = 60.0f;
        }

        float ft = filter_apply(&temp_filter, raw_t);
        float fh = filter_apply(&humid_filter, raw_h);

        // Gửi MQTT (không có gas)
        mqtt_publish_sensor(ft, fh, 0.0f, ft, fh, 0.0f);

        ESP_LOGI(TAG, "Temp=%.1f Hum=%.1f", ft, fh);

        esp_task_wdt_reset();
        vTaskDelayUntil(&xLastWakeTime, period);
    }
}

// ==================== Xử lý lệnh MQTT ====================
void process_command(const char *json) {
    // Điều khiển sensor power
    bool turn_on_sensor = sensor_powered;
    const char *p = strstr(json, "\"sensor_power\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            if (strstr(p, "true")) turn_on_sensor = true;
            else if (strstr(p, "false")) turn_on_sensor = false;
        }
    }
    if (turn_on_sensor != sensor_powered) {
        if (turn_on_sensor) sensor_power_on();
        else sensor_power_off();
    }

    // Điều khiển chế độ Peltier
    p = strstr(json, "\"peltier_mode\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            if (strstr(p, "\"cool\"")) {
                strcpy(peltier_mode, "cool");
                ESP_LOGI(TAG, "Peltier mode: COOL");
            } else if (strstr(p, "\"heat\"")) {
                strcpy(peltier_mode, "heat");
                ESP_LOGI(TAG, "Peltier mode: HEAT");
            } else if (strstr(p, "\"off\"")) {
                strcpy(peltier_mode, "off");
                ESP_LOGI(TAG, "Peltier mode: OFF");
            } else if (strstr(p, "\"auto\"")) {
                strcpy(peltier_mode, "auto");
                ESP_LOGI(TAG, "Peltier mode: AUTO");
            }
        }
    }
}

// ==================== Main ====================
void app_main(void) {
    ESP_LOGI(TAG, "E-nose system starting (no AI, no MQ2)");

    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 5000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_cfg);

    filter_init(&temp_filter);
    filter_init(&humid_filter);

    gpio_set_direction(SENSOR_PWR_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(SENSOR_PWR_GPIO, 0);
    sensor_powered = true;

    if (sht3x_init(I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO) != ESP_OK) {
        ESP_LOGE(TAG, "SHT3x init failed");
    }

    wifi_mqtt_init();
    mqtt_set_command_callback(process_command);

    xTaskCreatePinnedToCore(sensor_task, "sensor", 4096, NULL, 5, NULL, 1);
    xTaskCreate(peltier_control_task, "peltier", 2048, NULL, 3, NULL);
}
