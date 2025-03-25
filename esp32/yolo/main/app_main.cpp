/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <inttypes.h>
#include <stdio.h>

#include <forward_list>

#include "algo_yolo.hpp"
#include "app_camera.h"
// #include "app_lcd.h"
// #include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "esp_vfs_semihost.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/apps/netbiosns.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"
#include "utils/server.h"

static QueueHandle_t xQueueAIFrame = NULL;
static QueueHandle_t xQueueFrameOut = NULL;
static QueueHandle_t xQueueResults = NULL;
static QueueHandle_t xQueueEvent = NULL;

// static void print_frame(camera_fb_t *frame) {
//     printf("Frame: %d x %d, fmt: %d\n", frame->width, frame->height, frame->format);
// }
static const char *TAG = "MAIN";

#define HOST_NAME "esp32-cam-1"
#define MDNS_INSTANCE "IoT Device 1"

const char *WIFI_SSID = "****";
const char *WIFI_PASSWORD = "********";

void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                        void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Wi-Fi connecting...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi disconnected. Reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi connected. IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void wifi_init_sta(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize TCP/IP and Wi-Fi
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register Wi-Fi event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char *)wifi_config.sta.password, WIFI_PASSWORD);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Start Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi initialization complete.");
}
// Get the IP address of the ESP32
void get_ip_address() {
    esp_netif_t *netif =
        esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");  // Get netif handle for Wi-Fi STA
    if (netif == NULL) {
        ESP_LOGE(TAG, "Failed to get netif handle");
        return;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(netif, &ip_info);  // Get IP information
    if (ret == ESP_OK) {
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));  // Convert to string
        ESP_LOGI(TAG, "IP Address: %s", ip);
    } else {
        ESP_LOGE(TAG, "Failed to get IP info: %s", esp_err_to_name(ret));
    }
}

void print_reset_reason(void) {
    esp_reset_reason_t reason = esp_reset_reason();
    switch (reason) {
        case ESP_RST_UNKNOWN:
            ESP_LOGI(TAG, "Reset reason: Unknown");
            break;
        case ESP_RST_POWERON:
            ESP_LOGI(TAG, "Reset reason: Power on");
            break;
        case ESP_RST_EXT:
            ESP_LOGI(TAG, "Reset reason: External hardware");
            break;
        case ESP_RST_SW:
            ESP_LOGI(TAG, "Reset reason: Software");
            break;
        case ESP_RST_PANIC:
            ESP_LOGI(TAG, "Reset reason: Panic");
            break;
        case ESP_RST_INT_WDT:
            ESP_LOGI(TAG, "Reset reason: Watchdog");
            break;
        case ESP_RST_TASK_WDT:
            ESP_LOGI(TAG, "Reset reason: Task watchdog");
            break;
        case ESP_RST_WDT:
            ESP_LOGI(TAG, "Reset reason: Watchdog");
            break;
        case ESP_RST_DEEPSLEEP:
            ESP_LOGI(TAG, "Reset reason: Deep sleep");
            break;
        case ESP_RST_BROWNOUT:
            ESP_LOGI(TAG, "Reset reason: Brownout");
            break;
        case ESP_RST_SDIO:
            ESP_LOGI(TAG, "Reset reason: SDIO");
            break;
        default:
            ESP_LOGI(TAG, "Reset reason: Unknow");
            break;
    }
}

void wifi_scan_task(void *pvParameter) {
    // Configure Wi-Fi as a Station
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // ESP_ERROR_CHECK(esp_wifi_start());

    // Set scan configuration
    wifi_scan_config_t scan_config = {
        .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = true};

    ESP_LOGI(TAG, "Starting Wi-Fi scan...");
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));  // True for blocking scan

    // Get the scan results
    uint16_t number = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&number));
    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * number);
    if (ap_records == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for scan results");
        vTaskDelete(NULL);
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, ap_records));

    // Print scan results
    ESP_LOGI(TAG, "Total APs found: %d", number);
    for (int i = 0; i < number; i++) {
        ESP_LOGI(TAG, "SSID: %s, RSSI: %d, Channel: %d", ap_records[i].ssid, ap_records[i].rssi,
                 ap_records[i].primary);
    }

    // Clean up
    free(ap_records);
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());

    vTaskDelete(NULL);
}

extern "C" void app_main() {
    print_reset_reason();

    // Disable watchdog timer
    // esp_task_wdt_init(5, true);  // Set timeout to 5 seconds
    // esp_task_wdt_add(NULL);      // Add current task to WDT
    esp_log_level_set("wifi", ESP_LOG_DEBUG);
    esp_log_level_set("WIFI", ESP_LOG_DEBUG);
    esp_log_level_set("esp_netif", ESP_LOG_DEBUG);
    esp_log_level_set("to_jpg", ESP_LOG_DEBUG);
    // Initialize NVS

    xQueueAIFrame = xQueueCreate(2, sizeof(camera_fb_t *));
    xQueueFrameOut = xQueueCreate(2, sizeof(camera_fb_t *));
    // xQueueResults = xQueueCreate(2, sizeof(std::forward_list<yolo_t> *));
    xQueueEvent = xQueueCreate(2, sizeof(float *));

    register_camera(PIXFORMAT_RGB565, FRAMESIZE_QVGA, 10, xQueueAIFrame);
    // register_camera(PIXFORMAT_RGB565, FRAMESIZE_HVGA, 10, xQueueAIFrame);
    // sensor_t *s = esp_camera_sensor_get();
    // s->set_vflip(s, 1);
    register_algo_yolo(xQueueAIFrame, xQueueEvent, NULL, xQueueFrameOut, false);

    wifi_init_sta();

    // log result
    camera_fb_t *fb = NULL;
    // camera_fb_t *myfb = NULL;
    // std::forward_list<yolo_t> *obj_list = NULL;
    wifi_ap_record_t ap_info;
    int64_t initServer_time = -1;
    bool serverStart = false;
    int64_t stime;
    while (true) {
        if (!isStreaming || !serverStart) {
            // printf("Clearing camera, isStreaming = %d\n", isStreaming);

            if (xQueueReceive(xQueueFrameOut, &fb, portMAX_DELAY) == pdTRUE) {
                printf("AI out Cleared\n");
                esp_camera_fb_return(fb);
            } else {
                ESP_LOGW(TAG, "Cannot receive frame from xQueueFrameOut");
            }
        }
        if (xQueueReceive(xQueueAIFrame, &fb, portMAX_DELAY) == pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(100));
            printf("Camera Cleared\n");
            esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
        if (err == ESP_OK && initServer_time < 0) initServer_time = esp_timer_get_time();
        if (err == ESP_OK && !serverStart && (esp_timer_get_time() - initServer_time) / 1000 > 2) {
            serverStart = true;
            stime = esp_timer_get_time();

            printf("Connected to %s!\n", ap_info.ssid);
            startCameraServer(xQueueEvent, xQueueResults, xQueueFrameOut);
            ESP_LOGI(TAG, "startCameraServer Time: %lld", esp_timer_get_time() - stime);
            get_ip_address();
            continue;
        } else if (err != ESP_OK) {
            ESP_LOGI(TAG, "Still connecting to WiFi...");
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
