#include "wifi_mqtt.h"
#include <time.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "esp_sntp.h"   

#define WIFI_SSID      "Liti Garden Coffee"
#define WIFI_PASS      "camonquykhach"
#define MQTT_BROKER    "mqtt://192.168.1.124"// Điều chỉnh IP của bạn
#define DEVICE_ID      "dhieu"

static const char *TAG = "wifi_mqtt";
static esp_mqtt_client_handle_t mqtt_client = NULL;
static EventGroupHandle_t wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

static void (*command_callback)(const char *json) = NULL;

void mqtt_set_command_callback(void (*callback)(const char *json)) {
    command_callback = callback;
}

// Xử lý sự kiện MQTT
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected, subscribe command topic");
            esp_mqtt_client_subscribe(event->client, "e-nose/" DEVICE_ID "/command", 0);
            break;
        case MQTT_EVENT_DATA: {
            char topic[128];
            snprintf(topic, event->topic_len + 1, "%.*s", event->topic_len, event->topic);
            if (strstr(topic, "/command") && command_callback) {
                char payload[128];
                snprintf(payload, event->data_len + 1, "%.*s", event->data_len, event->data);
                command_callback(payload);
            }
            break;
        }
        default:
            break;
    }
}

// Khởi động MQTT client
static void mqtt_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER,
        .credentials.client_id = DEVICE_ID,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    ESP_LOGI(TAG, "MQTT started");
}

// Xử lý sự kiện WiFi
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        mqtt_start();
    }
}

// Khởi tạo WiFi + MQTT
void wifi_mqtt_init(void) {
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS
        }
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg);
    esp_wifi_start();

    wifi_event_group = xEventGroupCreate();
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected successfully");
        // Đồng bộ thời gian NTP
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
        int retry = 0;
        while (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < 10) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    } else {
        ESP_LOGE(TAG, "WiFi connection timeout");
    }
}

// Hàm publish dữ liệu cảm biến (chỉ 6 tham số, bỏ fan)
void mqtt_publish_sensor(float raw_temp, float raw_hum,
                         float cal_temp, float cal_hum) {
    if (mqtt_client == NULL) return;
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"device_id\":\"%s\",\"ts\":%lld,"
             "\"raw_temp\":%.2f,\"raw_hum\":%.2f"
             "\"temp\":%.2f,\"hum\":%.2f}",
             DEVICE_ID, (long long)time(NULL),
             raw_temp, raw_hum,
             cal_temp, cal_hum);
    esp_mqtt_client_publish(mqtt_client, "e-nose/nose01/data", payload, 0, 1, 0);
}
