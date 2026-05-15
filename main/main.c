/**
 * @file main.c
 * @brief ESP32Toy - ESP32 项目主程序
 * 
 * This is the main entry point for the ESP32Toy project.
 * 
 * Copyright (c) 2026
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "ESP32Toy";

/**
 * @brief 主应用任务
 */
void app_main(void)
{
    ESP_LOGI(TAG, "ESP32Toy 启动!");
    ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "SDK version: %s", esp_get_idf_version());

    while (1) {
        ESP_LOGI(TAG, "Hello from ESP32Toy!");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
