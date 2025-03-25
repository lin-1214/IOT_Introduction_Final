#include "algo_yolo.hpp"

#include <math.h>
#include <stdint.h>

#include <forward_list>

#include "base64.h"
#include "crane_new3_model_data.h"
#include "esp_http_client.h"
// #include "crane_new2_model_data.h"
// #include "crane_new_model_data.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fb_gfx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "isp.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
// #include "yolo_model_data.h"
#include "app_camera.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define LED_PIN 1      // Replace with the actual GPIO number for D0
#define SPEAKER_PIN 3  // Replace with the actual GPIO number for D2

void alert(void) {
    gpio_set_direction((gpio_num_t)LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)SPEAKER_PIN, GPIO_MODE_OUTPUT);

    for (int i = 0; i < 3; i++) {
        // Turn on LED and Speaker
        gpio_set_level((gpio_num_t)LED_PIN, 1);
        gpio_set_level((gpio_num_t)SPEAKER_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));  // Delay for 500ms

        // Turn off LED and Speaker
        gpio_set_level((gpio_num_t)LED_PIN, 0);
        gpio_set_level((gpio_num_t)SPEAKER_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));  // Delay for 500ms
    }
}
static const char *TAG = "yolo";

static QueueHandle_t xQueueFrameI = NULL;
static QueueHandle_t xQueueEvent = NULL;
static QueueHandle_t xQueueFrameO = NULL;
static QueueHandle_t xQueueResult = NULL;

static bool gEvent = true;
static bool gReturnFB = true;
static bool debug_mode = false;
static uint16_t image_change_threshold = 60;

// #define CONFIDENCE 25
int confidence = 40;
#define CONFIDENCE confidence
#define IOU 45

const uint16_t box_color[] = {0x0000, 0xFFFF, 0x07E0, 0x001F, 0xF800, 0xF81F,
                              0xFFE0, 0x07FF, 0x07FF, 0x07FF, 0x07FF};

#define ALERT_COLOR_IDX 4

std::forward_list<yolo_t> nms_get_obeject_topn(int8_t *dataset, uint16_t top_n, uint8_t threshold,
                                               uint8_t nms, uint16_t width, uint16_t height,
                                               int num_record, int8_t num_class, float scale,
                                               int zero_point);

// Globals, used for compatibility with Arduino-style sketches.
namespace {
const tflite::Model *model = nullptr;
tflite::MicroInterpreter *interpreter = nullptr;
TfLiteTensor *input = nullptr;
static std::forward_list<yolo_t> _yolo_list;
float border_x_ratio = -1;
/* Function to Send HTTP POST */
#define SERVER_URL "http://172.20.10.4:8080/esp32-message"
/* HTTP Event Handler */
esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key,
                     evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
    }
    return ESP_OK;
}
void http_post_message(void) {
    // JSON payload
    const char *post_data = "{\"message\":\"ALERT!\"}";

    // HTTP client configuration
    esp_http_client_config_t config = {
        .url = SERVER_URL,
        .event_handler = _http_event_handler,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    // Set HTTP method and headers
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    // Send the request
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    esp_err_t err = esp_http_client_perform(client);

    // Check response
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    // Cleanup
    esp_http_client_cleanup(client);
}

// In order to use optimized tensorflow lite kernels, a signed int8_t quantized
// model is preferred over the legacy unsigned model format. This means that
// throughout this project, input images must be converted from unisgned to
// signed format. The easiest and quickest way to convert from unsigned to
// signed 8-bit integers is to subtract 128 from the unsigned value to get a
// signed value.

#ifdef CONFIG_IDF_TARGET_ESP32S3
constexpr int scratchBufSize = 1024 * 1024;
#else
constexpr int scratchBufSize = 0;
#endif
// An area of memory to use for input, output, and intermediate arrays.
constexpr int kTensorArenaSize = 256 * 1024 + scratchBufSize + 1024;
static uint8_t *tensor_arena;  //[kTensorArenaSize]; // Maybe we should move this to external
}  // namespace
static void task_process_handler(void *arg) {
    camera_fb_t *frame = NULL;
    std::vector<uint8_t> last_frame_buf;
    alert();

    uint16_t h = input->dims->data[1];
    uint16_t w = input->dims->data[2];
    uint16_t c = input->dims->data[3];

    while (true) {
        if (gEvent) {
            if (xQueueReceive(xQueueFrameI, &frame, portMAX_DELAY)) {
                // bool is_approx = false;
                // if (!last_frame_buf.empty()) {
                //     int change_count = 0;
                //     for (int i = 0; i < frame->len; i++) {
                //         if (frame->buf[i] != last_frame_buf[i]) {
                //             change_count++;
                //         }
                //     }
                //     float mean_change = (float)change_count / frame->len * 100;
                //     is_approx = mean_change < image_change_threshold;
                //     if (debug_mode) {
                //         printf("Change count: %d\n", change_count);
                //         printf("Mean change: %f\n", mean_change);
                //         printf("Is approx: %d\n", is_approx);
                //     }
                // }
                // if (!last_frame_buf.empty()) {
                //     last_frame_buf.clear();
                // }
                // last_frame_buf.insert(last_frame_buf.end(), frame->buf, frame->buf + frame->len);
                // if (is_approx) {
                //     if (xQueueFrameO) {
                //         xQueueSend(xQueueFrameO, &frame, portMAX_DELAY);
                //     } else if (gReturnFB) {
                //         esp_camera_fb_return(frame);
                //     } else {
                //         free(frame);
                //     }

                //     if (xQueueResult) {
                //         std::forward_list<yolo_t> new_yolo_list = _yolo_list;
                //         xQueueSend(xQueueResult, &new_yolo_list, portMAX_DELAY);
                //     }
                //     continue;
                // }

                int dsp_start_time = esp_timer_get_time() / 1000;
                _yolo_list.clear();

                if (c == 1)
                    rgb565_to_gray(input->data.uint8, frame->buf, frame->height, frame->width, h, w,
                                   ROTATION_UP);
                else if (c == 3)
                    rgb565_to_rgb888(input->data.uint8, frame->buf, frame->height, frame->width, h,
                                     w, ROTATION_UP);

                int dsp_end_time = esp_timer_get_time() / 1000;

                if (debug_mode) {
                    printf("Begin output\n");
                    printf(
                        "Format: {\"height\": %d, \"width\": %d, \"channels\": %d, \"model\": "
                        "\"yolo\"}\r\n",
                        h, w, c);
                }

                for (int i = 0; i < input->bytes; i++) {
                    input->data.int8[i] = input->data.uint8[i] - 128;
                }

                // Run the model on this input and make sure it succeeds.
                int start_time = esp_timer_get_time() / 1000;

                if (kTfLiteOk != interpreter->Invoke()) {
                    MicroPrintf("Invoke failed.");
                }

                int end_time = esp_timer_get_time() / 1000;

                vTaskDelay(10 / portTICK_PERIOD_MS);

                TfLiteTensor *output = interpreter->output(0);

                // Get the results of the inference attempt
                float scale =
                    ((TfLiteAffineQuantization *)(output->quantization.params))->scale->data[0];
                int zero_point = ((TfLiteAffineQuantization *)(output->quantization.params))
                                     ->zero_point->data[0];

                uint32_t records = output->dims->data[1];
                uint32_t num_class = output->dims->data[2] - OBJECT_T_INDEX;

                _yolo_list = nms_get_obeject_topn(output->data.int8, records, CONFIDENCE, IOU, w, h,
                                                  records, num_class, scale, zero_point);
                if (debug_mode) {
                    printf("Predictions (DSP: %d ms., Classification: %d ms., Anomaly: %d ms.): \n",
                           (dsp_end_time - dsp_start_time), (end_time - start_time), 0);
                }
                bool found = false;

                if (std::distance(_yolo_list.begin(), _yolo_list.end()) > 0) {
                    int index = 0;
                    found = true;
                    if (debug_mode) {
                        printf("    Objects found: %d\n",
                               std::distance(_yolo_list.begin(), _yolo_list.end()));
                        printf("    Objects:\n");
                        printf("    [\n");
                    }
                    for (auto &yolo : _yolo_list) {
                        yolo.x = uint16_t(float(yolo.x) / float(w) * float(frame->width));
                        yolo.y = uint16_t(float(yolo.y) / float(h) * float(frame->height));
                        yolo.w = uint16_t(float(yolo.w) / float(w) * float(frame->width));
                        yolo.h = uint16_t(float(yolo.h) / float(h) * float(frame->height));
                        yolo.alert = false;
                        if (yolo.confidence > CONFIDENCE) {
                            if (border_x_ratio > 0.0 && yolo.target == 1) {
                                uint16_t target_x = uint16_t(float(frame->width) * border_x_ratio);
                                printf("Target X: %d\n", target_x);
                                printf("Yolo X: %d\n", yolo.x);
                                if (yolo.x - yolo.w / 2 < target_x &&
                                    yolo.x + yolo.w / 2 > target_x) {
                                    yolo.alert = true;
                                    printf("Cross the line!!!\n");
                                    alert();
                                    http_post_message();
                                    printf("Alert the server!!!\n");
                                }
                            }
                            int color_idx = yolo.alert ? ALERT_COLOR_IDX : yolo.target;
                            fb_gfx_drawRect2(
                                frame, yolo.x - yolo.w / 2, yolo.y - yolo.h / 2, yolo.w, yolo.h,
                                box_color[color_idx % (sizeof(box_color) / sizeof(box_color[0]))],
                                4);
                        }

                        if (debug_mode) {
                            printf(
                                "        {\"class\": \"%d\", \"x\": %d, \"y\": %d, \"w\": %d, "
                                "\"h\": "
                                "%d, \"confidence\": %d},\n",
                                yolo.target, yolo.x, yolo.y, yolo.w, yolo.h, yolo.confidence);
                        }
                        index++;
                    }
                    if (debug_mode) {
                        printf("    ]\n");
                    }
                }

                if (!found) {
                    printf("No objects found\n");
                } else {
                    printf("Found!\n");
                }
                if (debug_mode) {
                    printf("End output\n");
                }
                if (xQueueFrameO) {
                    xQueueSend(xQueueFrameO, &frame, portMAX_DELAY);
                } else if (gReturnFB) {
                    esp_camera_fb_return(frame);
                } else {
                    free(frame);
                }
            }

            // typedef struct {
            //     uint16_t x;
            //     uint16_t y;
            //     uint16_t w;
            //     uint16_t h;
            //     uint8_t confidence;
            //     uint8_t target;
            //     bool alert;
            // } yolo_t;
            if (xQueueResult) {
                // std::forward_list<yolo_t> new_yolo_list;
                // for (auto &yolo : _yolo_list) {
                //     new_yolo_list.emplace_front(yolo);
                // }
                // xQueueSend(xQueueResult, &new_yolo_list, portMAX_DELAY);
            }
        }
    }
}

static void task_event_handler(void *arg) {
    float _x_ratio = 0.0;
    int _confidence = 0.0;
    while (true) {
        if (xQueueReceive(xQueueEvent, &_x_ratio, portMAX_DELAY)) {
            printf("Modifying Event: %f\n", _x_ratio);

            if (_x_ratio == -1) {
                border_x_ratio = -1;
                continue;
            }
            if (_x_ratio == -2) {
                confidence = 25;
                continue;
            }

            // float part for x ration
            // int part for confidence
            _confidence = int(_x_ratio);
            _x_ratio = _x_ratio - _confidence;

            if (_confidence >= 0) confidence = _confidence;
            if (_x_ratio >= 0.01 && _x_ratio <= 1) border_x_ratio = _x_ratio;
            printf("Border: %f, Confidence: %d\n", border_x_ratio, confidence);
        }
    }
}

int register_algo_yolo(const QueueHandle_t frame_i, const QueueHandle_t event,
                       const QueueHandle_t result, const QueueHandle_t frame_o,
                       const bool camera_fb_return) {
    xQueueFrameI = frame_i;
    xQueueFrameO = frame_o;
    xQueueEvent = event;
    xQueueResult = result;
    gReturnFB = camera_fb_return;

    // get model (.tflite) from flash
    model = tflite::GetModel(g_crane_new3_model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        MicroPrintf(
            "Model provided is schema version %d not equal to supported "
            "version %d.",
            model->version(), TFLITE_SCHEMA_VERSION);
        return -1;
    }

    if (tensor_arena == NULL) {
        tensor_arena =
            (uint8_t *)heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (tensor_arena == NULL) {
        printf("Couldn't allocate memory of %d bytes\n", kTensorArenaSize);
        return -1;
    }

    static tflite::MicroMutableOpResolver<19> micro_op_resolver;
    micro_op_resolver.AddConv2D();
    micro_op_resolver.AddDepthwiseConv2D();
    micro_op_resolver.AddReshape();
    micro_op_resolver.AddPad();
    micro_op_resolver.AddPadV2();
    micro_op_resolver.AddAdd();
    micro_op_resolver.AddSub();
    micro_op_resolver.AddRelu();
    micro_op_resolver.AddMean();
    micro_op_resolver.AddMaxPool2D();
    micro_op_resolver.AddConcatenation();
    micro_op_resolver.AddQuantize();
    micro_op_resolver.AddTranspose();
    micro_op_resolver.AddLogistic();
    micro_op_resolver.AddMul();
    micro_op_resolver.AddSplitV();
    micro_op_resolver.AddStridedSlice();
    micro_op_resolver.AddResizeNearestNeighbor();
    micro_op_resolver.AddGather();

    // Build an interpreter to run the model with.
    // NOLINTNEXTLINE(runtime-global-variables)
    static tflite::MicroInterpreter static_interpreter(model, micro_op_resolver, tensor_arena,
                                                       kTensorArenaSize);
    interpreter = &static_interpreter;

    // Allocate memory from the tensor_arena for the model's tensors.
    TfLiteStatus allocate_status = interpreter->AllocateTensors();
    if (allocate_status != kTfLiteOk) {
        MicroPrintf("AllocateTensors() failed");
        return -1;
    }

    // Get information about the memory area to use for the model's input.
    input = interpreter->input(0);

    xTaskCreatePinnedToCore(task_process_handler, TAG, 4 * 1024, NULL, 5, NULL, 0);
    if (xQueueEvent) xTaskCreatePinnedToCore(task_event_handler, TAG, 4 * 1024, NULL, 5, NULL, 1);

    return 0;
}

#define CLIP(x, y, z) (x < y) ? y : ((x > z) ? z : x)

static bool _object_comparator_reverse(yolo_t &oa, yolo_t &ob) {
    return oa.confidence < ob.confidence;
}

static bool _object_nms_comparator(yolo_t &oa, yolo_t &ob) { return oa.confidence > ob.confidence; }

static bool _object_comparator(yolo_t &oa, yolo_t &ob) { return oa.x > ob.x; }

bool _object_remove(yolo_t &obj) { return (obj.confidence == 0); }

static uint16_t _overlap(float x1, float w1, float x2, float w2) {
    uint16_t l1 = x1 - w1 / 2;
    uint16_t l2 = x2 - w2 / 2;
    uint16_t left = l1 > l2 ? l1 : l2;
    uint16_t r1 = x1 + w1 / 2;
    uint16_t r2 = x2 + w2 / 2;
    uint16_t right = r1 < r2 ? r1 : r2;
    return right - left;
}

void _soft_nms_obeject_detection(std::forward_list<yolo_t> &yolo_obj_list, uint8_t nms) {
    std::forward_list<yolo_t>::iterator max_box_obj;
    yolo_obj_list.sort(_object_nms_comparator);
    for (std::forward_list<yolo_t>::iterator it = yolo_obj_list.begin(); it != yolo_obj_list.end();
         ++it) {
        uint16_t area = it->w * it->h;
        for (std::forward_list<yolo_t>::iterator itc = std::next(it, 1); itc != yolo_obj_list.end();
             ++itc) {
            if (itc->confidence == 0) {
                continue;
            }
            uint16_t iw = _overlap(itc->x, itc->w, it->x, it->w);
            if (iw > 0) {
                uint16_t ih = _overlap(itc->y, itc->h, it->y, it->h);
                if (ih > 0) {
                    float ua = float(itc->w * itc->h + area - iw * ih);
                    float ov = iw * ih / ua;
                    if (int(float(ov) * 100) >= nms) {
                        itc->confidence = 0;
                    }
                }
            }
        }
    }
    yolo_obj_list.remove_if(_object_remove);
    return;
}

void _hard_nms_obeject_count(std::forward_list<yolo_t> &yolo_obj_list, uint8_t nms) {
    std::forward_list<yolo_t>::iterator max_box_obj;
    yolo_obj_list.sort(_object_nms_comparator);
    for (std::forward_list<yolo_t>::iterator it = yolo_obj_list.begin(); it != yolo_obj_list.end();
         ++it) {
        uint16_t area = it->w * it->h;
        for (std::forward_list<yolo_t>::iterator itc = std::next(it, 1); itc != yolo_obj_list.end();
             ++itc) {
            if (itc->confidence == 0) {
                continue;
            }
            uint16_t iw = _overlap(itc->x, itc->w, it->x, it->w);
            if (iw > 0) {
                uint16_t ih = _overlap(itc->y, itc->h, it->y, it->h);
                if (ih > 0) {
                    float ua = float(itc->w * itc->h + area - iw * ih);
                    float ov = iw * ih / ua;
                    if (int(float(ov) * 100) >= nms) {
                        itc->confidence = 0;
                    }
                }
            }
        }
    }
    yolo_obj_list.remove_if(_object_remove);

    return;
}

std::forward_list<yolo_t> nms_get_obeject_topn(int8_t *dataset, uint16_t top_n, uint8_t threshold,
                                               uint8_t nms, uint16_t width, uint16_t height,
                                               int num_record, int8_t num_class, float scale,
                                               int zero_point) {
    bool rescale = scale < 0.1 ? true : false;
    std::forward_list<yolo_t> yolo_obj_list[num_class];
    int16_t num_obj[num_class] = {0};
    int16_t num_element = num_class + OBJECT_T_INDEX;
    for (int i = 0; i < num_record; i++) {
        float confidence = float(dataset[i * num_element + OBJECT_C_INDEX] - zero_point) * scale;
        confidence = rescale ? confidence * 100 : confidence;
        if (int(float(confidence)) >= threshold) {
            yolo_t obj;
            int8_t max = -128;
            obj.target = 0;

            for (int j = 0; j < num_class; j++) {
                if (max < dataset[i * num_element + OBJECT_T_INDEX + j]) {
                    max = dataset[i * num_element + OBJECT_T_INDEX + j];
                    obj.target = j;
                }
            }

            float x = float(dataset[i * num_element + OBJECT_X_INDEX] - zero_point) * scale;
            float y = float(dataset[i * num_element + OBJECT_Y_INDEX] - zero_point) * scale;
            float w = float(dataset[i * num_element + OBJECT_W_INDEX] - zero_point) * scale;
            float h = float(dataset[i * num_element + OBJECT_H_INDEX] - zero_point) * scale;

            if (rescale) {
                obj.x = CLIP(int(x * width), 0, width);
                obj.y = CLIP(int(y * height), 0, height);
                obj.w = CLIP(int(w * width), 0, width);
                obj.h = CLIP(int(h * height), 0, height);
            } else {
                obj.x = CLIP(int(x), 0, width);
                obj.y = CLIP(int(y), 0, height);
                obj.w = CLIP(int(w), 0, width);
                obj.h = CLIP(int(h), 0, height);
            }
            obj.w = (obj.x + obj.w) > width ? (width - obj.x) : obj.w;
            obj.h = (obj.y + obj.h) > height ? (height - obj.y) : obj.h;
            obj.confidence = confidence;
            if (num_obj[obj.target] >= top_n) {
                yolo_obj_list[obj.target].sort(_object_comparator_reverse);
                if (obj.confidence > yolo_obj_list[obj.target].front().confidence) {
                    yolo_obj_list[obj.target].pop_front();
                    yolo_obj_list[obj.target].emplace_front(obj);
                }
            } else {
                yolo_obj_list[obj.target].emplace_front(obj);
                num_obj[obj.target]++;
            }
        }
    }

    std::forward_list<yolo_t> result;

    for (int i = 0; i < num_class; i++) {
        if (!yolo_obj_list[i].empty()) {
            _soft_nms_obeject_detection(yolo_obj_list[i], nms);
            result.splice_after(result.before_begin(), yolo_obj_list[i]);
        }
    }

    result.sort(_object_comparator);  // left to right

    return result;
}