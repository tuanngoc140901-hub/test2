#include <stdio.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "sht3x_driver.h"
#include "filter_lib.h"
#include "wifi_mqtt.h"
#include "mlp_weights.h"

static const char *TAG = "main";

// ==================== Bộ lọc trung vị ====================
static median_filter_t temp_filter;
static median_filter_t humid_filter;
static median_filter_t mq2_filter;

// ==================== ADC oneshot ====================
static adc_oneshot_unit_handle_t adc1_handle;
#define MQ2_ADC_CHANNEL   ADC_CHANNEL_6   // GPIO34
#define NTC_ADC_CHANNEL   ADC_CHANNEL_7   // GPIO35

// ==================== I2C cho SHT3x ====================
#define I2C_MASTER_SDA_IO   21
#define I2C_MASTER_SCL_IO   22

// ==================== Điều khiển nguồn cảm biến (MOSFET P-channel) ====================
#define SENSOR_PWR_GPIO    GPIO_NUM_26    // LOW = bật
static bool sensor_powered = true;

// ==================== Peltier & NTC ====================
#define PELTIER_PWR_GPIO   GPIO_NUM_27   // HIGH = bật
#define TEMP_LIMIT          50.0f
#define TEMP_HYSTERESIS     5.0f
static bool peltier_on = false;

// ==================== Quạt làm mát – LEDC PWM tuyến tính ====================
#define FAN_PWM_GPIO      GPIO_NUM_18
#define LEDC_TIMER         LEDC_TIMER_0
#define LEDC_MODE          LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL       LEDC_CHANNEL_0
#define LEDC_DUTY_RES      LEDC_TIMER_8_BIT   // 0-255
#define LEDC_FREQUENCY     25000              // 25kHz siêu âm
#define TEMP_MIN           28.0f
#define TEMP_MAX           42.0f
#define FAN_MIN_DUTY       40                // ~15%, giữ quạt quay nhẹ

static float g_calibrated_temp = 25.0f;       // Nhiệt độ calibrated từ AI
static SemaphoreHandle_t g_temp_mutex = NULL; // Bảo vệ biến chung

static bool fan_manual_override = false;      // Cờ override thủ công
static uint32_t fan_manual_duty = 0;          // Duty khi override

// ==================== Forward declarations ====================
static void sensor_power_on(void);
static void sensor_power_off(void);
static void i2c_recovery(void);
static void adc_init(void);
static int read_adc_raw(adc_channel_t channel);
static float read_mq2_raw(void);
static float read_ntc_temp(void);
static void process_command(const char *json);

// ==================== AI – Mạng nơ-ron ====================
static inline float relu(float x) {
    return x > 0 ? x : 0;
}

static void mlp_predict(float input[3], float output[3]) {
    float hidden[6];
    for (int i = 0; i < 6; i++) {
        float sum = b1[i];
        for (int j = 0; j < 3; j++) {
            sum += input[j] * W1[j][i];
        }
        hidden[i] = relu(sum);
    }
    for (int i = 0; i < 3; i++) {
        float sum = b2[i];
        for (int j = 0; j < 6; j++) {
            sum += hidden[j] * W2[j][i];
        }
        output[i] = sum;
    }
}

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
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, MQ2_ADC_CHANNEL, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, NTC_ADC_CHANNEL, &chan_cfg));
}

static int read_adc_raw(adc_channel_t channel) {
    int raw;
    adc_oneshot_read(adc1_handle, channel, &raw);
    return raw;
}

static float read_mq2_raw(void) {
    int raw = read_adc_raw(MQ2_ADC_CHANNEL);
    return raw * 3.3f / 4095.0f;
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

// ==================== LEDC khởi tạo ====================
static void ledc_init(void) {
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t chan_conf = {
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = FAN_PWM_GPIO,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&chan_conf);
}

// ==================== Task bảo vệ Peltier ====================
void peltier_control_task(void *arg) {
    esp_task_wdt_add(NULL);
    gpio_set_direction(PELTIER_PWR_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(PELTIER_PWR_GPIO, 0);

    while (1) {
        float temp = read_ntc_temp();
        if (temp > TEMP_LIMIT) {
            gpio_set_level(PELTIER_PWR_GPIO, 0);
            if (peltier_on) {
                peltier_on = false;
                ESP_LOGW(TAG, "Peltier OFF (overheat: %.1f)", temp);
            }
        } else if (temp < TEMP_LIMIT - TEMP_HYSTERESIS) {
            if (!peltier_on) {
                peltier_on = true;
                gpio_set_level(PELTIER_PWR_GPIO, 1);
                ESP_LOGI(TAG, "Peltier ON (%.1f)", temp);
            }
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ==================== Task điều khiển quạt ====================
void fan_control_task(void *arg) {
    esp_task_wdt_add(NULL);
    uint32_t duty = 255;
    float local_temp = 0.0f;

    while (1) {
        duty = 255;  // DEBUG: ép 100%
        ESP_LOGI(TAG, "Fan DEBUG: duty=%lu", duty);

        /* // Tự động (sau khi test xong bỏ comment)
        if (xSemaphoreTake(g_temp_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            local_temp = g_calibrated_temp;
            xSemaphoreGive(g_temp_mutex);
        }
        if (fan_manual_override) {
            duty = fan_manual_duty;
        } else {
            if (local_temp <= TEMP_MIN) {
                duty = FAN_MIN_DUTY;
            } else if (local_temp >= TEMP_MAX) {
                duty = 255;
            } else {
                float ratio = (local_temp - TEMP_MIN) / (TEMP_MAX - TEMP_MIN);
                duty = FAN_MIN_DUTY + (uint32_t)(ratio * (255.0f - FAN_MIN_DUTY));
            }
        }
        */

        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ==================== Task cảm biến chính ====================
void sensor_task(void *arg) {
    esp_task_wdt_add(NULL);
    float raw_t, raw_h, raw_gas;
    float input[3], output[3];

    adc_init();
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

        raw_gas = read_mq2_raw();

        float ft = filter_apply(&temp_filter, raw_t);
        float fh = filter_apply(&humid_filter, raw_h);
        float fg = filter_apply(&mq2_filter, raw_gas);

        input[0] = ft; input[1] = fh; input[2] = fg;
        mlp_predict(input, output);

        if (xSemaphoreTake(g_temp_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            g_calibrated_temp = output[0];
            xSemaphoreGive(g_temp_mutex);
        }

        mqtt_publish_sensor(ft, fh, fg, output[0], output[1], output[2]);

        ESP_LOGI(TAG, "Raw: T=%.1f H=%.1f G=%.2f -> Cal: T=%.1f H=%.1f G=%.2f",
                 ft, fh, fg, output[0], output[1], output[2]);

        esp_task_wdt_reset();
        vTaskDelayUntil(&xLastWakeTime, period);
    }
}

// ==================== Xử lý lệnh MQTT ====================
void process_command(const char *json) {
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

    bool turn_on_peltier = peltier_on;
    p = strstr(json, "\"peltier\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            if (strstr(p, "true")) turn_on_peltier = true;
            else if (strstr(p, "false")) turn_on_peltier = false;
        }
    }
    if (turn_on_peltier != peltier_on) {
        gpio_set_level(PELTIER_PWR_GPIO, turn_on_peltier ? 1 : 0);
        peltier_on = turn_on_peltier;
        ESP_LOGI(TAG, "Peltier %s", turn_on_peltier ? "ON" : "OFF");
    }

    bool new_override = false;
    uint32_t new_duty = 0;
    p = strstr(json, "\"fan\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            if (strstr(p, "true")) {
                new_override = true;
                new_duty = 255;
            } else if (strstr(p, "false")) {
                new_override = true;
                new_duty = 0;
            }
        }
    }
    p = strstr(json, "\"fan_auto\"");
    if (p) {
        p = strchr(p, ':');
        if (p && strstr(p, "true")) {
            new_override = false;
        }
    }

    if (new_override != fan_manual_override || new_duty != fan_manual_duty) {
        fan_manual_override = new_override;
        fan_manual_duty = new_duty;
        if (new_override) {
            ESP_LOGI(TAG, "Fan manual override: duty=%lu", new_duty);
        } else {
            ESP_LOGI(TAG, "Fan back to auto mode");
        }
    }
}

// ==================== Main ====================
void app_main(void) {
    ESP_LOGI(TAG, "E-nose AI system starting");

    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 5000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_cfg);

    filter_init(&temp_filter);
    filter_init(&humid_filter);
    filter_init(&mq2_filter);

    gpio_set_direction(SENSOR_PWR_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(SENSOR_PWR_GPIO, 0);
    sensor_powered = true;

    g_temp_mutex = xSemaphoreCreateMutex();
    assert(g_temp_mutex != NULL);

    ledc_init();

    if (sht3x_init(I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO) != ESP_OK) {
        ESP_LOGE(TAG, "SHT3x init failed");
    }

    wifi_mqtt_init();
    mqtt_set_command_callback(process_command);

    xTaskCreatePinnedToCore(sensor_task, "sensor", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(fan_control_task, "fan_ctrl", 3072, NULL, 4, NULL, 1);
    xTaskCreate(peltier_control_task, "peltier", 2048, NULL, 3, NULL);
}