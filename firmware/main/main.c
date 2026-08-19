#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "hw_codec.h"
#include "hw_display.h"
#include "audio_capture.h"
#include "audio_encoder.h"
#include "network_tx.h"
#include "secrets.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

static const char *TAG = "babyphone_main";

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "Wi-Fi initialization started. Connecting to %s", WIFI_SSID);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Babyphone Sender Firmware Starting...");
    
    wifi_init_sta();

    // Create Ringbuffers (No-Split format)
    // 10 PCM frames buffer (10 * 640 bytes)
    RingbufHandle_t pcm_ringbuf = xRingbufferCreate(6400, RINGBUF_TYPE_NOSPLIT);
    // 20 RTP frames buffer (~20 * 80 bytes)
    RingbufHandle_t rtp_ringbuf = xRingbufferCreate(1600, RINGBUF_TYPE_NOSPLIT);

    // Initialize Hardware
    ESP_ERROR_CHECK(hw_codec_init());
    ESP_ERROR_CHECK(hw_display_init());
    
    // Start Audio Pipeline Tasks
    ESP_ERROR_CHECK(network_tx_init(rtp_ringbuf));
    ESP_ERROR_CHECK(audio_encoder_init(pcm_ringbuf, rtp_ringbuf));
    ESP_ERROR_CHECK(audio_capture_init(pcm_ringbuf));

    // Show a green screen to indicate successful boot
    hw_display_test_pattern();

    
    while (1) {
        // Main loop can sleep, work is done in FreeRTOS tasks
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
