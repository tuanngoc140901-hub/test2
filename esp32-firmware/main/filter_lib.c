#include "filter_lib.h"
#include <string.h>
#include <stdlib.h>

void filter_init(median_filter_t *f) {
    memset(f, 0, sizeof(*f));
}

float filter_apply(median_filter_t *f, float value) {
    // Thêm vào buffer
    f->buffer[f->index] = value;
    f->index = (f->index + 1) % FILTER_WINDOW_SIZE;
    if (f->count < FILTER_WINDOW_SIZE) f->count++;

    // Tạo mảng tạm và sắp xếp để lấy trung vị
    float sorted[FILTER_WINDOW_SIZE];
    memcpy(sorted, f->buffer, f->count * sizeof(float));
    // Sắp xếp nổi bọt cho đơn giản (dùng ít bộ nhớ)
    for (int i = 0; i < f->count - 1; i++) {
        for (int j = 0; j < f->count - i - 1; j++) {
            if (sorted[j] > sorted[j+1]) {
                float tmp = sorted[j];
                sorted[j] = sorted[j+1];
                sorted[j+1] = tmp;
            }
        }
    }
    if (f->count % 2 == 1) {
        return sorted[f->count / 2];
    } else {
        return (sorted[f->count/2 - 1] + sorted[f->count/2]) / 2.0f;
    }
}
