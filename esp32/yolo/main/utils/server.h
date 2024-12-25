#ifndef __SERVER_H__
#define __SERVER_H__
// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// #include "esp32-hal-ledc.h"

#include <freertos/queue.h>

#include "cJSON.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "fb_gfx.h"
#include "img_converters.h"
#include "sdkconfig.h"
#include "utils.h"

#define STAG "SERVER"
// Face DetectK

#define QUANT_TYPE 0  // if set to 1 => very large firmware, very slow, reboots when streaming...

#define COLOR_WHITE 0x00FFFFFF
#define COLOR_BLACK 0x00000000
#define COLOR_RED 0x000000FF
#define COLOR_GREEN 0x0000FF00
#define COLOR_BLUE 0x00FF0000
#define COLOR_YELLOW (COLOR_RED | COLOR_GREEN)
#define COLOR_CYAN (COLOR_BLUE | COLOR_GREEN)
#define COLOR_PURPLE (COLOR_BLUE | COLOR_RED)

#define CONFIG_LED_ILLUMINATOR_ENABLED 1

#define LED_LEDC_GPIO 22  // configure LED pin
#define CONFIG_LED_MAX_INTENSITY 255

int led_duty = 200;
bool isStreaming = false;
// int16_t confidence = 25;
enum border_method_t { BORDER_EDIT, BORDER_DELETE };

typedef struct {
    httpd_req_t *req;
    size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

typedef struct {
    QueueHandle_t frame;
    QueueHandle_t part;
    QueueHandle_t event;
} video_context_t;

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len) {
    jpg_chunking_t *j = (jpg_chunking_t *)arg;
    if (!index) {
        j->len = 0;
    }
    if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK) {
        return 0;
    }
    j->len += len;
    return len;
}
static esp_err_t stream_handler(httpd_req_t *req) {
    printf("Streaming request IN!\n");

    camera_fb_t *fb = NULL;
    struct timeval _timestamp;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char part_buf[128];
    size_t out_len = 0, out_width = 0, out_height = 0;
    uint8_t *out_buf = NULL;
    bool s = false;
    video_context_t *ctx = (video_context_t *)req->user_ctx;

    QueueHandle_t frame = ctx->frame;
    printf("Frame Queue: %p\n", frame);

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        return res;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "60");

    isStreaming = true;
    printf("Start Streaming\n");
    int64_t stime = esp_timer_get_time();
    while (true) {
        if (xQueueReceive(frame, &fb, portMAX_DELAY) != pdTRUE) {
            esp_camera_fb_return(fb);
            ESP_LOGI(STAG, "Failed to receive frame from queue\n");
            res = ESP_FAIL;
        } else {
            _timestamp.tv_sec = fb->timestamp.tv_sec;
            _timestamp.tv_usec = fb->timestamp.tv_usec;
            ESP_LOGI(STAG, "Frame format: %d\n", fb->format);
            stime = esp_timer_get_time();
            if (fb->format == PIXFORMAT_RGB565) {
                ESP_LOGI(STAG, "Frame format: PIXFORMAT_RGB565\n");
                ESP_LOGI("MEMORY", "Free heap: %d", esp_get_free_heap_size());

                s = fmt2jpg(fb->buf, fb->len, fb->width, fb->height, PIXFORMAT_RGB565, 80,
                            &_jpg_buf, &_jpg_buf_len);

                ESP_LOGI(STAG, "frame2jpg Time: %lld", esp_timer_get_time() - stime);
                esp_camera_fb_return(fb);
                fb = NULL;
                if (!s) {
                    ESP_LOGI(STAG, "fmt2jpg failed");
                    res = ESP_FAIL;
                }
            } else {
                ESP_LOGI(STAG, "Frame format: %d\n", fb->format);
                out_len = fb->width * fb->height * 3;
                out_width = fb->width;
                out_height = fb->height;
                out_buf = (uint8_t *)malloc(out_len);
                if (!out_buf) {
                    ESP_LOGI(STAG, "out_buf malloc failed");
                    res = ESP_FAIL;
                } else {
                    ESP_LOGI(STAG, "2 rgb888");
                    s = fmt2rgb888(fb->buf, fb->len, fb->format, out_buf);
                    esp_camera_fb_return(fb);
                    fb = NULL;
                    if (!s) {
                        free(out_buf);
                        ESP_LOGI(STAG, "To rgb888 failed");
                        res = ESP_FAIL;
                    } else {
                        s = fmt2jpg(out_buf, out_len, out_width, out_height, PIXFORMAT_RGB888, 80,
                                    &_jpg_buf, &_jpg_buf_len);
                        free(out_buf);
                        if (!s) {
                            ESP_LOGI(STAG, "fmt2jpg failed");
                            res = ESP_FAIL;
                        }
                    }
                }
            }

            if (res == ESP_OK) {
                res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
                ESP_LOGI(STAG, "send _STREAM_BOUNDARY");
            }
            if (res == ESP_OK) {
                size_t hlen = snprintf(part_buf, 128, _STREAM_PART, _jpg_buf_len, _timestamp.tv_sec,
                                       _timestamp.tv_usec);
                res = httpd_resp_send_chunk(req, part_buf, hlen);
                ESP_LOGI(STAG, "send part_buf");
            }
            if (res == ESP_OK) {
                res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
                ESP_LOGI(STAG, "send _jpg_buf");
            }
            if (fb) {
                esp_camera_fb_return(fb);
                fb = NULL;
                _jpg_buf = NULL;
            } else if (_jpg_buf) {
                free(_jpg_buf);
                _jpg_buf = NULL;
            }
            if (res != ESP_OK) {
                printf("Send frame failed\n");
                break;
            }
            ESP_LOGI(STAG, "Process time: %lld", esp_timer_get_time() - stime);
        }
    }
    printf("Streaming End\n");

    isStreaming = false;
    return res;
}
static esp_err_t border_handler(httpd_req_t *req) {
    printf("Border request IN!\n");

    char buf[100];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (json == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    const cJSON *method = cJSON_GetObjectItemCaseSensitive(json, "method");
    const cJSON *x_ratio = cJSON_GetObjectItemCaseSensitive(json, "xPosition");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "streamId");

    if (!cJSON_IsString(method) || !cJSON_IsNumber(x_ratio) || !cJSON_IsNumber(id)) {
        cJSON_Delete(json);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    video_context_t *ctx = (video_context_t *)req->user_ctx;
    float event = x_ratio->valuedouble;
    if (strcmp(method->valuestring, "clear") == 0) {
        event = -1;
    }

    printf("Border method: %s, x_ratio: %f, streamId: %d\n", method->valuestring, event,
           id->valueint);
    if (xQueueSend(ctx->event, &event, portMAX_DELAY) != pdTRUE) {
        cJSON_Delete(json);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON_Delete(json);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    printf("Border request Success!\n");

    return httpd_resp_send(req, NULL, 0);
}
static esp_err_t confidence_handler(httpd_req_t *req) {
    printf("Confidence request IN!\n");

    char buf[100];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (json == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    const cJSON *__cond = cJSON_GetObjectItemCaseSensitive(json, "confidence");

    if (!cJSON_IsNumber(__cond)) {
        cJSON_Delete(json);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    video_context_t *ctx = (video_context_t *)req->user_ctx;
    float event = int(__cond->valuedouble);

    printf("Confidence: %f\n", event);
    if (xQueueSend(ctx->event, &event, portMAX_DELAY) != pdTRUE) {
        cJSON_Delete(json);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON_Delete(json);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    printf("Confidence request Success!\n");

    return httpd_resp_send(req, NULL, 0);
}
static esp_err_t parse_get(httpd_req_t *req, char **obuf) {
    char *buf = NULL;
    size_t buf_len = 0;

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = (char *)malloc(buf_len);
        if (!buf) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            *obuf = buf;
            return ESP_OK;
        }
        free(buf);
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static esp_err_t cmd_handler(httpd_req_t *req) {
    char *buf = NULL;
    char variable[32];
    char value[32];

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) != ESP_OK ||
        httpd_query_key_value(buf, "val", value, sizeof(value)) != ESP_OK) {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(buf);

    int val = atoi(value);
    sensor_t *s = esp_camera_sensor_get();
    int res = 0;

    if (!strcmp(variable, "framesize")) {
        if (s->pixformat == PIXFORMAT_JPEG) {
            res = s->set_framesize(s, (framesize_t)val);
        }
    } else if (!strcmp(variable, "quality")) {
        res = s->set_quality(s, val);
    } else if (!strcmp(variable, "contrast")) {
        res = s->set_contrast(s, val);
    } else if (!strcmp(variable, "brightness")) {
        res = s->set_brightness(s, val);
    } else if (!strcmp(variable, "saturation")) {
        res = s->set_saturation(s, val);
    } else if (!strcmp(variable, "gainceiling")) {
        res = s->set_gainceiling(s, (gainceiling_t)val);
    } else if (!strcmp(variable, "colorbar")) {
        res = s->set_colorbar(s, val);
    } else if (!strcmp(variable, "awb")) {
        res = s->set_whitebal(s, val);
    } else if (!strcmp(variable, "agc")) {
        res = s->set_gain_ctrl(s, val);
    } else if (!strcmp(variable, "aec")) {
        res = s->set_exposure_ctrl(s, val);
    } else if (!strcmp(variable, "hmirror")) {
        res = s->set_hmirror(s, val);
    } else if (!strcmp(variable, "vflip")) {
        res = s->set_vflip(s, val);
    } else if (!strcmp(variable, "awb_gain")) {
        res = s->set_awb_gain(s, val);
    } else if (!strcmp(variable, "agc_gain")) {
        res = s->set_agc_gain(s, val);
    } else if (!strcmp(variable, "aec_value")) {
        res = s->set_aec_value(s, val);
    } else if (!strcmp(variable, "aec2")) {
        res = s->set_aec2(s, val);
    } else if (!strcmp(variable, "dcw")) {
        res = s->set_dcw(s, val);
    } else if (!strcmp(variable, "bpc")) {
        res = s->set_bpc(s, val);
    } else if (!strcmp(variable, "wpc")) {
        res = s->set_wpc(s, val);
    } else if (!strcmp(variable, "raw_gma")) {
        res = s->set_raw_gma(s, val);
    } else if (!strcmp(variable, "lenc")) {
        res = s->set_lenc(s, val);
    } else if (!strcmp(variable, "special_effect")) {
        res = s->set_special_effect(s, val);
    } else if (!strcmp(variable, "wb_mode")) {
        res = s->set_wb_mode(s, val);
    } else if (!strcmp(variable, "ae_level")) {
        res = s->set_ae_level(s, val);
    } else if (!strcmp(variable, "led_intensity")) {
        led_duty = val;
    } else {
        printf("Unknown command: %s", variable);
        res = -1;
    }

    if (res < 0) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static int print_reg(char *p, sensor_t *s, uint16_t reg, uint32_t mask) {
    return sprintf(p, "\"0x%x\":%u,", reg, s->get_reg(s, reg, mask));
}

static esp_err_t status_handler(httpd_req_t *req) {
    static char json_response[1024];

    sensor_t *s = esp_camera_sensor_get();
    char *p = json_response;
    *p++ = '{';

    if (s->id.PID == OV5640_PID || s->id.PID == OV3660_PID) {
        for (int reg = 0x3400; reg < 0x3406; reg += 2) {
            p += print_reg(p, s, reg, 0xFFF);  // 12 bit
        }
        p += print_reg(p, s, 0x3406, 0xFF);

        p += print_reg(p, s, 0x3500, 0xFFFF0);  // 16 bit
        p += print_reg(p, s, 0x3503, 0xFF);
        p += print_reg(p, s, 0x350a, 0x3FF);   // 10 bit
        p += print_reg(p, s, 0x350c, 0xFFFF);  // 16 bit

        for (int reg = 0x5480; reg <= 0x5490; reg++) {
            p += print_reg(p, s, reg, 0xFF);
        }

        for (int reg = 0x5380; reg <= 0x538b; reg++) {
            p += print_reg(p, s, reg, 0xFF);
        }

        for (int reg = 0x5580; reg < 0x558a; reg++) {
            p += print_reg(p, s, reg, 0xFF);
        }
        p += print_reg(p, s, 0x558a, 0x1FF);  // 9 bit
    } else if (s->id.PID == OV2640_PID) {
        p += print_reg(p, s, 0xd3, 0xFF);
        p += print_reg(p, s, 0x111, 0xFF);
        p += print_reg(p, s, 0x132, 0xFF);
    }

    p += sprintf(p, "\"xclk\":%u,", s->xclk_freq_hz / 1000000);
    p += sprintf(p, "\"pixformat\":%u,", s->pixformat);
    p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
    p += sprintf(p, "\"quality\":%u,", s->status.quality);
    p += sprintf(p, "\"brightness\":%d,", s->status.brightness);
    p += sprintf(p, "\"contrast\":%d,", s->status.contrast);
    p += sprintf(p, "\"saturation\":%d,", s->status.saturation);
    p += sprintf(p, "\"sharpness\":%d,", s->status.sharpness);
    p += sprintf(p, "\"special_effect\":%u,", s->status.special_effect);
    p += sprintf(p, "\"wb_mode\":%u,", s->status.wb_mode);
    p += sprintf(p, "\"awb\":%u,", s->status.awb);
    p += sprintf(p, "\"awb_gain\":%u,", s->status.awb_gain);
    p += sprintf(p, "\"aec\":%u,", s->status.aec);
    p += sprintf(p, "\"aec2\":%u,", s->status.aec2);
    p += sprintf(p, "\"ae_level\":%d,", s->status.ae_level);
    p += sprintf(p, "\"aec_value\":%u,", s->status.aec_value);
    p += sprintf(p, "\"agc\":%u,", s->status.agc);
    p += sprintf(p, "\"agc_gain\":%u,", s->status.agc_gain);
    p += sprintf(p, "\"gainceiling\":%u,", s->status.gainceiling);
    p += sprintf(p, "\"bpc\":%u,", s->status.bpc);
    p += sprintf(p, "\"wpc\":%u,", s->status.wpc);
    p += sprintf(p, "\"raw_gma\":%u,", s->status.raw_gma);
    p += sprintf(p, "\"lenc\":%u,", s->status.lenc);
    p += sprintf(p, "\"hmirror\":%u,", s->status.hmirror);
    p += sprintf(p, "\"dcw\":%u,", s->status.dcw);
    p += sprintf(p, "\"colorbar\":%u", s->status.colorbar);
    p += sprintf(p, ",\"led_intensity\":%u", led_duty);

    *p++ = '}';
    *p++ = 0;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_response, strlen(json_response));
}

static int parse_get_var(char *buf, const char *key, int def) {
    char _int[16];
    if (httpd_query_key_value(buf, key, _int, sizeof(_int)) != ESP_OK) {
        return def;
    }
    return atoi(_int);
}

void startCameraServer(const QueueHandle_t event, const QueueHandle_t result,
                       const QueueHandle_t frame_o) {
    printf("================Starting Camera Server================\n");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;

    video_context_t *context = (video_context_t *)calloc(1, sizeof(video_context_t));
    context->frame = frame_o;
    context->part = result;
    context->event = event;

    httpd_uri_t status_uri = {.uri = "/status",
                              .method = HTTP_GET,
                              .handler = status_handler,
                              .user_ctx = (void *)context};

    httpd_uri_t stream_uri = {.uri = "/stream",
                              .method = HTTP_GET,
                              .handler = stream_handler,
                              .user_ctx = (void *)context};

    httpd_uri_t cmd_uri = {
        .uri = "/control", .method = HTTP_GET, .handler = cmd_handler, .user_ctx = (void *)context};

    httpd_uri_t border_uri = {.uri = "/border",
                              .method = HTTP_POST,
                              .handler = border_handler,
                              .user_ctx = (void *)context};

    ring_buff_init(&ring_buff, 20);

    // log_i("Starting web server on port: '%d'", config.server_port);
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &status_uri);
        httpd_register_uri_handler(camera_httpd, &cmd_uri);
        httpd_register_uri_handler(camera_httpd, &border_uri);
    }

    config.server_port += 1;
    config.ctrl_port += 1;
    isStreaming = false;
    // log_i("Starting stream server on port: '%d'", config.server_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
    }
    ESP_LOGI(STAG, "Registering URI: %s", border_uri.uri);

    printf("==================================================\n");
}

// void setupLedFlash(int pin) { ledcAttach(pin, 5000, 8); }

#endif  // __SERVER_H__