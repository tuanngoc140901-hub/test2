#include <stdio.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_adc/adc_oneshot.h"
#include "sht3x_driver.h"
#include "filter_lib.h"
#include "wifi_mqtt.h"

static const char *TAG = "main";

// ==================== Bộ lọc trung vị ====================
static median_filter_t temp_filter;
static median_filter_t humid_filter;

// ==================== ADC oneshot cho NTC ====================
static adc_oneshot_unit_handle_t adc1_handle = NULL;
#define NTC_ADC_CHANNEL   ADC_CHANNEL_7   // GPIO35

// ==================== I2C cho SHT3x ====================
#define I2C_MASTER_SDA_IO   21
#define I2C_MASTER_SCL_IO   22

// ==================== Điều khiển nguồn cảm biến (MOSFET P-channel) ====================
#define SENSOR_PWR_GPIO    GPIO_NUM_26    // LOW = bật
static bool sensor_powered = true;
static bool target_sensor_power = true;   
static bool sensor_power_changing = false; 

// ==================== Peltier (MOSFET FR120N trên D18) ====================
#define PELTIER_PWR_GPIO   GPIO_NUM_18   // HIGH = bật Peltier
#define TEMP_LOW_THRESHOLD   20.0f
#define TEMP_HIGH_THRESHOLD  30.0f
#define TEMP_HYSTERESIS      2.0f

static char peltier_mode[8] = "off";
static bool peltier_on = false;

// ==================== Định nghĩa các bit sự kiện Event Bits ====================
#define BIT_EVENT_1SEC      (1 << 0)   // Kích hoạt chu kỳ đọc SHT3x (1 giây)
#define BIT_EVENT_20MS      (1 << 1)   // SHT3x đo xong / Nguồn ổn định xong
#define BIT_EVENT_2SEC      (1 << 2)   // Kích hoạt chu kỳ quét Peltier (2 giây)

static gptimer_handle_t sensor_timer = NULL;
static TimerHandle_t sht3x_delay_timer = NULL;
static TaskHandle_t sensor_task_handle = NULL;
static TaskHandle_t peltier_task_handle = NULL;

// ==================== Forward declarations ====================
static void i2c_recovery_no_delay(void);
static void adc_init(void);
static int read_adc_raw(adc_channel_t channel);
static float read_ntc_temp(void);
static void process_command(const char *json);

// ==================== GPTimer 1 giây (Hardware ISR) ====================
static bool IRAM_ATTR sensor_timer_isr(gptimer_handle_t timer,
                                       const gptimer_alarm_event_data_t *edata,
                                       void *user_ctx) {
    BaseType_t high_task_awoken = pdFALSE;
    
    // Mỗi 1 giây: Gửi tín hiệu cho luồng Sensor Task
    xTaskNotifyFromISR(sensor_task_handle, BIT_EVENT_1SEC, eSetBits, &high_task_awoken);
    
    // Đếm chu kỳ 2 giây: Gửi tín hiệu cho luồng Peltier Task
    static uint8_t sec_counter = 0;
    sec_counter++;
    if (sec_counter >= 2) {
        sec_counter = 0;
        if (peltier_task_handle) {
            xTaskNotifyFromISR(peltier_task_handle, BIT_EVENT_2SEC, eSetBits, &high_task_awoken);
        }
    }
    
    return (high_task_awoken == pdTRUE);
}

static void sensor_timer_init(void) {
    gptimer_config_t cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,   // 1 tick = 1 µs
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&cfg, &sensor_timer));
    gptimer_event_callbacks_t cbs = { .on_alarm = sensor_timer_isr };
    gptimer_register_event_callbacks(sensor_timer, &cbs, NULL);
    gptimer_alarm_config_t alarm = {
        .alarm_count = 1000000,     // Ngắt mỗi 1.000.000 µs = 1 giây
        .flags.auto_reload_on_alarm = true,
    };
    gptimer_set_alarm_action(sensor_timer, &alarm);
    ESP_ERROR_CHECK(gptimer_enable(sensor_timer));
    ESP_ERROR_CHECK(gptimer_start(sensor_timer));
}

// ==================== Software Timer Callback ====================
static void sht3x_delay_callback(TimerHandle_t xTimer) {
    if (sensor_task_handle) {
        xTaskNotify(sensor_task_handle, BIT_EVENT_20MS, eSetBits);
    }
}

// ==================== Phục hồi I2C TUYỆT ĐỐI KHÔNG DELAY ====================
static void i2c_recovery_no_delay(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << I2C_MASTER_SDA_IO) | (1ULL << I2C_MASTER_SCL_IO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    // Lật chân GPIO liên tiếp tạo xung clock tốc độ cao nhả bus tức thì, không dùng delay_us
    for (int i = 0; i < 9; i++) {
        gpio_set_level(I2C_MASTER_SCL_IO, 0);
        gpio_set_level(I2C_MASTER_SCL_IO, 1);
    }
    sht3x_deinit();
    
    if (sensor_powered) {
        sht3x_init(I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
    }
}

// ==================== ADC & Đọc nhiệt độ NTC ====================
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
    int raw = 0;
    if (adc1_handle != NULL) {
        adc_oneshot_read(adc1_handle, channel, &raw);
    }
    return raw;
}

static float read_ntc_temp(void) {
    int raw = read_adc_raw(NTC_ADC_CHANNEL);
    if (raw <= 0 || raw >= 4095) return 25.0f;
    float voltage = raw * 3.3f / 4095.0f;
    float r_fixed = 10000.0f;
    float r_ntc = r_fixed * (3.3f - voltage) / voltage;
    float T0 = 298.15f;
    float R0 = 10000.0f;
    float B = 3950.0f;
    float steinhart = logf(r_ntc / R0) / B + 1.0f / T0;
    return 1.0f / steinhart - 273.15f;
}

// ==================== Logic Peltier điều khiển ====================
static void update_peltier(void) {
    float temp = read_ntc_temp();
    bool should_on = false;

    if (strcmp(peltier_mode, "cool") == 0) {
        if (temp > TEMP_HIGH_THRESHOLD + TEMP_HYSTERESIS) should_on = true;
        else if (temp < TEMP_HIGH_THRESHOLD - TEMP_HYSTERESIS) should_on = false;
        else should_on = peltier_on;
    } else if (strcmp(peltier_mode, "heat") == 0) {
        if (temp < TEMP_LOW_THRESHOLD - TEMP_HYSTERESIS) should_on = true;
        else if (temp > TEMP_LOW_THRESHOLD + TEMP_HYSTERESIS) should_on = false;
        else should_on = peltier_on;
    } else if (strcmp(peltier_mode, "auto") == 0) {
        if (temp > TEMP_HIGH_THRESHOLD || temp < TEMP_LOW_THRESHOLD) should_on = true;
        else should_on = false;
    } else {
        should_on = false;
    }

    peltier_on = should_on;
    gpio_set_level(PELTIER_PWR_GPIO, peltier_on ? 1 : 0);
    
    static bool last_peltier_state = false;
    if (peltier_on != last_peltier_state) {
        ESP_LOGI(TAG, "Peltier State Changed: %s (temp=%.1f, mode=%s)", peltier_on ? "ON" : "OFF", temp, peltier_mode);
        last_peltier_state = peltier_on;
    }
}

// ==================== Peltier Task (100% Ngắt phần cứng kích hoạt) ====================
void peltier_control_task(void *arg) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PELTIER_PWR_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE, // Giữ an toàn trạng thái ban đầu của FET
    };
    gpio_config(&io_conf);
    gpio_set_level(PELTIER_PWR_GPIO, 0);

    esp_task_wdt_add(NULL);
    uint32_t notified_value = 0;

    while (1) {
        // Ngủ đông hoàn toàn cho đến khi có tín hiệu ngắt 2 giây từ Hardware GPTimer
        xTaskNotifyWait(0x00, ULONG_MAX, &notified_value, portMAX_DELAY);

        if (notified_value & BIT_EVENT_2SEC) {
            update_peltier();
        }
        
        esp_task_wdt_reset();
    }
}

// ==================== Sensor Task (100% Không Delay) ====================
void sensor_task(void *arg) {
    esp_task_wdt_add(NULL);
    
    float raw_t = 25.0f, raw_h = 60.0f;
    uint32_t notified_value = 0;

    while (1) {
        // Ngủ đông hoàn toàn tiêu tốn 0% CPU khi rảnh rỗi
        xTaskNotifyWait(0x00, ULONG_MAX, &notified_value, portMAX_DELAY);

        // --- KIỂM TRA TRẠNG THÁI NGUỒN (XỬ LÝ ĐỆM KHÔNG CHẶN) ---
        if (target_sensor_power != sensor_powered && !sensor_power_changing) {
            if (target_sensor_power) {
                // Yêu cầu BẬT nguồn: Kéo chân cứng kích nguồn ngay
                gpio_set_level(SENSOR_PWR_GPIO, 0);
                sensor_power_changing = true; 
                // Tận dụng phần mềm hẹn giờ định thời đúng 100ms để đợi nguồn ổn định (không dùng delay)
                xTimerChangePeriod(sht3x_delay_timer, pdMS_TO_TICKS(100), 0);
                ESP_LOGI(TAG, "Sensor power ON sequence started, waiting stable...");
            } else {
                // Yêu cầu TẮT nguồn: Thực hiện ngắt logic tức thì
                gpio_config_t io_conf = {
                    .pin_bit_mask = (1ULL << I2C_MASTER_SDA_IO) | (1ULL << I2C_MASTER_SCL_IO),
                    .mode = GPIO_MODE_INPUT,
                };
                gpio_config(&io_conf);
                sht3x_deinit();
                gpio_set_level(SENSOR_PWR_GPIO, 1);
                sensor_powered = false;
                ESP_LOGI(TAG, "Sensor power OFF executed.");
            }
        }

        // Trường hợp 1: Tín hiệu định kỳ mỗi 1 giây từ Hardware GPTimer
        if (notified_value & BIT_EVENT_1SEC) {
            if (sensor_powered && !sensor_power_changing) {
                sht3x_start_measurement();
                // Đặt Software Timer về lại 20ms tiêu chuẩn để chờ chip SHT chuyển đổi dữ liệu xong
                xTimerChangePeriod(sht3x_delay_timer, pdMS_TO_TICKS(20), 0);
            } else if (!sensor_powered) {
                // Khi tắt nguồn, trả về ngay dữ liệu fallback tĩnh mà không gọi I2C
                float ft = filter_apply(&temp_filter, 25.0f);
                float fh = filter_apply(&humid_filter, 60.0f);
                mqtt_publish_sensor(ft, fh, ft, fh);
                ESP_LOGI(TAG, "Sensor OFF Mode - Temp=%.1f Hum=%.1f", ft, fh);
            }
        }

        // Trường hợp 2: Tín hiệu từ Software Timer báo hết thời gian chờ
        if (notified_value & BIT_EVENT_20MS) {
            if (sensor_power_changing) {
                // Đã ngắt/đợi xong 100ms ổn định nguồn -> Tiến hành kích hoạt Driver SHT3x
                sht3x_init(I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
                sensor_powered = true;
                sensor_power_changing = false;
                ESP_LOGI(TAG, "Sensor power ON stable. SHT3x logic active.");
            } else if (sensor_powered) {
                // Đọc kết quả đo thông thường
                esp_err_t ret = sht3x_read_result(&raw_t, &raw_h);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "I2C read error, triggering recovery");
                    i2c_recovery_no_delay();
                    raw_t = 25.0f;
                    raw_h = 60.0f;
                }

                float ft = filter_apply(&temp_filter, raw_t);
                float fh = filter_apply(&humid_filter, raw_h);

                mqtt_publish_sensor(ft, fh, ft, fh);
                ESP_LOGI(TAG, "Temp=%.1f Hum=%.1f", ft, fh);
            }
        }

        esp_task_wdt_reset();
    }
}

// ==================== Hàm Callback nhận dữ liệu MQTT từ broker ====================
void process_command(const char *json) {
    const char *p = strstr(json, "\"sensor_power\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            if (strstr(p, "true")) target_sensor_power = true;
            else if (strstr(p, "false")) target_sensor_power = false;
        }
    }

    p = strstr(json, "\"peltier_mode\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            if (strstr(p, "\"cool\"")) strcpy(peltier_mode, "cool");
            else if (strstr(p, "\"heat\"")) strcpy(peltier_mode, "heat");
            else if (strstr(p, "\"off\"")) strcpy(peltier_mode, "off");
            else if (strstr(p, "\"auto\"")) strcpy(peltier_mode, "auto");
            ESP_LOGI(TAG, "Peltier mode updated to: %s", peltier_mode);
        }
    }
}

// ==================== Main Khởi Chạy ====================
void app_main(void) {
    ESP_LOGI(TAG, "E-nose system starting (Zero-Delay Architecture)...");

    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 5000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_cfg);

    filter_init(&temp_filter);
    filter_init(&humid_filter);
    adc_init();

    gpio_set_direction(SENSOR_PWR_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(SENSOR_PWR_GPIO, 0); 
    sensor_powered = true;
    target_sensor_power = true;

    if (sht3x_init(I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO) != ESP_OK) {
        ESP_LOGE(TAG, "SHT3x init failed");
    }

    wifi_mqtt_init();
    mqtt_set_command_callback(process_command);

    // 1. Khởi tạo Software Timer (Chu kỳ mặc định ban đầu đặt tạm 20ms)
    sht3x_delay_timer = xTimerCreate("sht3x_dly", pdMS_TO_TICKS(20), pdFALSE, NULL, sht3x_delay_callback);

    // 2. Tạo luồng Task và lấy Task Handle chính xác tuyệt đối
    xTaskCreatePinnedToCore(sensor_task, "sensor", 4096, NULL, 5, &sensor_task_handle, 1);
    xTaskCreate(peltier_control_task, "peltier", 2048, NULL, 3, &peltier_task_handle);

    // 3. Khởi chạy Hardware GPTimer ở bước cuối cùng khi các cấu trúc luồng đã sẵn sàng nhận Notification
    sensor_timer_init();
}
