#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"

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
#include "freertos/event_groups.h"

static const char *TAG = "babyphone_main";

// FreeRTOS event group to signal when we are connected
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Disconnected from AP, retrying connection...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

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
    
    // Disable Wi-Fi power save to prevent multicast packet loss and route dropping
    // Must be called AFTER esp_wifi_start()
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    
    // Keep default TX power to ensure stable Wi-Fi connection

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    // Wait until the connection is established (WIFI_CONNECTED_BIT)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", WIFI_SSID);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Babyphone Sender Firmware Starting...");
    // Initialize Hardware FIRST to turn off the backlight and save power
    // Otherwise, transmitting Wi-Fi with the backlight on causes a hardware brownout!
    ESP_ERROR_CHECK(hw_codec_init());
    ESP_ERROR_CHECK(hw_display_init());

    wifi_init_sta();
    // 10 PCM frames buffer (10 * 640 bytes)
    RingbufHandle_t pcm_ringbuf = xRingbufferCreate(8192, RINGBUF_TYPE_NOSPLIT);
    RingbufHandle_t rtp_ringbuf = xRingbufferCreate(8192, RINGBUF_TYPE_NOSPLIT);

    // Start Audio Pipeline Tasks
    ESP_ERROR_CHECK(network_tx_init(rtp_ringbuf));
    ESP_ERROR_CHECK(audio_encoder_init(pcm_ringbuf, rtp_ringbuf));
    ESP_ERROR_CHECK(audio_capture_init(pcm_ringbuf));

    // Show a green screen to indicate successful boot
    hw_display_test_pattern();

    
    // Keep the task alive but stop blinking
    while (1) {
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}
