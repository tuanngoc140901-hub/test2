#pragma once
#include <stdint.h>

#define FILTER_WINDOW_SIZE 7

typedef struct {
    float buffer[FILTER_WINDOW_SIZE];
    uint8_t index;
    uint8_t count;
} median_filter_t;

void filter_init(median_filter_t *f);
float filter_apply(median_filter_t *f, float value);
