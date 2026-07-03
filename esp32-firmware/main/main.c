#include <stdio.h>
#include <time.h>
#include <math.h> // Thêm thư viện logf
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "driver/gptimer.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "sht3x_driver.h"
#include "filter_lib.h"
#include "wifi_mqtt.h"

static const char *TAG = "main";

// ==================== Bộ lọc trung vị ====================
static median_filter_t temp_filter;
static median_filter_t humid_filter;

// ==================== ADC (NTC) ====================
static adc_oneshot_unit_handle_t adc1_handle;
#define NTC_ADC_CHANNEL    ADC_CHANNEL_7   // GPIO35

// ==================== I2C SHT3x ====================
#define I2C_MASTER_SDA_IO   21
#define I2C_MASTER_SCL_IO   22

// ==================== Peltier (FR120N GPIO18) ====================
#define PELTIER_PWR_GPIO    GPIO_NUM_18

// Ngưỡng nhiệt độ SHT3x để bật/tắt Peltier
#define TEMP_ON_THRESHOLD   25.5f
#define TEMP_OFF_THRESHOLD  24.5f

// Ngưỡng bảo vệ quá nhiệt NTC
#define NTC_MAX_TEMP        50.0f

// ==================== Biến toàn cục (Volatile giúp tối ưu hóa Compiler) ====================
// Vì ghi/đọc float trên ESP32 là atomic nên không cần dùng Mutex làm chậm Timer Task
static volatile float g_current_temp = 25.0f;         

// ==================== Event bits cho sensor_task ====================
#define BIT_EVENT_1SEC      (1 << 0)
#define BIT_EVENT_20MS      (1 << 1)

// ==================== Timers & Task handles ====================
static gptimer_handle_t sensor_timer = NULL;       // GPTimer 1 giây
static TimerHandle_t sht3x_delay_timer = NULL;     // Software Timer 20ms
static TimerHandle_t peltier_timer = NULL;         // Software Timer 2 giây
static TaskHandle_t sensor_task_handle = NULL;

// ==================== Forward declarations ====================
static void adc_init(void);
static float read_ntc_temp(void);
static void peltier_control_callback(TimerHandle_t xTimer);

// ==================== GPTimer 1 giây (Hardware) ====================
static bool IRAM_ATTR sensor_timer_isr(gptimer_handle_t timer,
                                       const gptimer_alarm_event_data_t *edata,
                                       void *user_ctx) {
    BaseType_t high_task_awoken = pdFALSE;
    xTaskNotifyFromISR(sensor_task_handle, BIT_EVENT_1SEC, eSetBits, &high_task_awoken);
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
        .alarm_count = 1000000,     // 1 giây
        .flags.auto_reload_on_alarm = true,
    };
    gptimer_set_alarm_action(sensor_timer, &alarm);
    ESP_ERROR_CHECK(gptimer_enable(sensor_timer));
    ESP_ERROR_CHECK(gptimer_start(sensor_timer));
    ESP_LOGI(TAG, "Sensor timer (1 Hz) started");
}

// ==================== Software Timer 20 ms ====================
static void sht3x_delay_callback(TimerHandle_t xTimer) {
    if (sensor_task_handle) {
        xTaskNotify(sensor_task_handle, BIT_EVENT_20MS, eSetBits);
    }
}

// ==================== ADC khởi tạo ====================
static void adc_init(void) {
    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc1_handle));
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, NTC_ADC_CHANNEL, &chan_cfg));
}

static float read_ntc_temp(void) {
    int raw;
    if (adc_oneshot_read(adc1_handle, NTC_ADC_CHANNEL, &raw) != ESP_OK) {
        return 25.0f; // Fallback an toàn nếu ADC lỗi
    }
    float voltage = raw * 3.3f / 4095.0f;
    if (voltage <= 0.0f || voltage >= 3.3f) return 25.0f; // Tránh chia cho 0
    
    float r_fixed = 10000.0f;
    float r_ntc = r_fixed * (3.3f - voltage) / voltage;
    float T0 = 298.15f;
    float R0 = 10000.0f;
    float B = 3950.0f;
    float steinhart = logf(r_ntc / R0) / B + 1.0f / T0;
    return 1.0f / steinhart - 273.15f;
}

// ==================== Sensor Task ====================
void sensor_task(void *arg) {
    esp_task_wdt_add(NULL);
    sensor_task_handle = xTaskGetCurrentTaskHandle();

    float raw_t, raw_h;
    uint32_t notified_value = 0;

    while (1) {
        // Luôn ở trạng thái Blocked hoàn toàn (0% CPU), chỉ thức dậy khi có Event
        xTaskNotifyWait(0x00, ULONG_MAX, &notified_value, portMAX_DELAY);

        // Sự kiện 1 giây (Từ Hardware GPTimer): bắt đầu kích hoạt cảm biến đo
        if (notified_value & BIT_EVENT_1SEC) {
            sht3x_start_measurement();
            // Đã tối ưu: Thay ChangePeriod bằng xTimerStart cho One-shot timer
            xTimerStart(sht3x_delay_timer, 0); 
        }

        // Sự kiện 20ms (Từ Software Timer): đọc kết quả sau khi đo xong
        if (notified_value & BIT_EVENT_20MS) {
            esp_err_t ret = sht3x_read_result(&raw_t, &raw_h);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "I2C error, using fallback");
                raw_t = 25.0f;
                raw_h = 60.0f;
            }

            float ft = filter_apply(&temp_filter, raw_t);
            float fh = filter_apply(&humid_filter, raw_h);

            // Ghi trực tiếp (Atomic), không cần Mutex gây nguy cơ Deadlock cho Timer Task
            g_current_temp = ft;

            // Gửi MQTT (Non-blocking queue ngầm định bên trong thư viện)
            mqtt_publish_sensor(ft, fh, ft, fh);
            ESP_LOGI(TAG, "Temp=%.1f Hum=%.1f", ft, fh);
        }

        esp_task_wdt_reset();
    }
}

// ==================== Peltier Control (Software Timer 2 giây) ====================
static void peltier_control_callback(TimerHandle_t xTimer) {
    // Đọc nhiệt độ bảo vệ NTC từ ADC (Hàm oneshot_read là hàm non-blocking phần cứng)
    float ntc_temp = read_ntc_temp();

    // Đọc an toàn trực tiếp từ biến toàn cục
    float sht3x_temp = g_current_temp;

    bool peltier_on = false;

    // Bảo vệ quá nhiệt độc lập: NTC > 50°C -> ngắt lập tức
    if (ntc_temp > NTC_MAX_TEMP) {
        peltier_on = false;
        ESP_LOGW(TAG, "NTC Overheat Warning (%.1f°C), Peltier FORCED OFF", ntc_temp);
    } else {
        // Thuật toán điều khiển Hysteresis (vùng trễ chống đóng cắt liên tục)
        static bool last_peltier_state = false;
        if (sht3x_temp >= TEMP_ON_THRESHOLD) {
            peltier_on = true;
        } else if (sht3x_temp <= TEMP_OFF_THRESHOLD) {
            peltier_on = false;
        } else {
            peltier_on = last_peltier_state; 
        }
        last_peltier_state = peltier_on;
    }

    // Xuất tín hiệu điều khiển ra Transistor/MOSFET điều khiển nguồn Peltier
    gpio_set_level(PELTIER_PWR_GPIO, peltier_on ? 1 : 0);
}

// ==================== Main ====================
void app_main(void) {
    ESP_LOGI(TAG, "E-nose with Peltier & NTC protection (No Delays/No Mutex)");

    // Khởi tạo Watchdog hệ thống 5 giây
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 5000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_cfg);

    // Khởi tạo bộ lọc trung vị
    filter_init(&temp_filter);
    filter_init(&humid_filter);

    // Khởi tạo ngoại vi ADC cho NTC
    adc_init();

    // Khởi tạo GPIO điều khiển Peltier 
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PELTIER_PWR_GPIO),
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(PELTIER_PWR_GPIO, 0);

    // Khởi tạo I2C cho SHT3x
    if (sht3x_init(I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO) != ESP_OK) {
        ESP_LOGE(TAG, "SHT3x init failed");
    }

    // Kết nối WiFi & MQTT Broker
    wifi_mqtt_init();

    // 1. Tạo Software Timer 20ms cho sensor (One-shot)
    sht3x_delay_timer = xTimerCreate("sht3x_dly", pdMS_TO_TICKS(20), pdFALSE, NULL, sht3x_delay_callback);

    // 2. Tạo Software Timer 2 giây cho việc kiểm soát Peltier (Auto-reload)
    peltier_timer = xTimerCreate("peltier", pdMS_TO_TICKS(2000), pdTRUE, NULL, peltier_control_callback);
    xTimerStart(peltier_timer, 0);

    // 3. Tạo luồng Sensor Task xử lý dữ liệu
    xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, &sensor_task_handle);

    // 4. Kích hoạt Hardware Timer sau cùng khi toàn bộ hệ thống Event-driven đã sẵn sàng
    sensor_timer_init();
}
