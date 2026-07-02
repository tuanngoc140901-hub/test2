#pragma once
#include <stdbool.h>

void wifi_mqtt_init(void);
void mqtt_set_command_callback(void (*callback)(const char *json));
void mqtt_publish_sensor(float raw_temp, float raw_hum, 
                         float cal_temp, float cal_hum);
void mqtt_set_command_callback(void (*callback)(const char *json));
