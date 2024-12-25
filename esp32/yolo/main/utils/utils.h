#ifndef __CAMERAWEBSERVER_UTILS__
#define __CAMERAWEBSERVER_UTILS__
#include "esp_camera.h"
#define U_TAG "UTILS"

typedef struct {
    size_t size;   // number of values used for buffering
    size_t index;  // current value index
    size_t count;  // value count
    int sum;
    int *values;  // array to be filled with values
} ring_buff_t;

static ring_buff_t ring_buff;

static ring_buff_t *ring_buff_init(ring_buff_t *buff, size_t sample_size) {
    memset(buff, 0, sizeof(ring_buff_t));

    buff->values = (int *)malloc(sample_size * sizeof(int));
    if (!buff->values) {
        return NULL;
    }
    memset(buff->values, 0, sample_size * sizeof(int));

    buff->size = sample_size;
    return buff;
}
static int ring_buff_run(ring_buff_t *buff, int value) {
    // append value & return running mean of current window
    if (!buff->values) {
        return value;
    }
    buff->sum -= buff->values[buff->index];
    buff->values[buff->index] = value;
    buff->sum += buff->values[buff->index];
    buff->index++;
    buff->index = buff->index % buff->size;
    if (buff->count < buff->size) {
        buff->count++;
    }
    return buff->sum / buff->count;
}

camera_fb_t *copy_camera_fb(const camera_fb_t *src) {
    if (!src) {
        ESP_LOGE(U_TAG, "Source frame is NULL");
        return NULL;
    }

    // Allocate memory for the new frame structure
    camera_fb_t *copy = (camera_fb_t *)malloc(sizeof(camera_fb_t));
    if (!copy) {
        ESP_LOGE(U_TAG, "Failed to allocate memory for frame copy");
        return NULL;
    }

    // Copy the structure fields
    *copy = *src;

    // Allocate memory for the buffer and copy the data
    copy->buf = (uint8_t *)malloc(src->len);
    if (!copy->buf) {
        ESP_LOGE(U_TAG, "Failed to allocate memory for frame buffer copy");
        free(copy);
        return NULL;
    }
    memcpy(copy->buf, src->buf, src->len);

    ESP_LOGI(U_TAG, "Frame copied successfully. Size: %zu bytes", src->len);
    return copy;
}
// Function to free the copied frame
void free_camera_fb_copy(camera_fb_t *copy) {
    if (copy) {
        free(copy->buf);
        free(copy);
    }
}

#endif