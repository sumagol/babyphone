#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "hw_codec.h"
#include "hw_display.h"
#include "ui.h"
#include "audio_capture.h"
#include "audio_encoder.h"
#include "network_tx.h"
#include "wifi_manager.h"
#include "freertos/event_groups.h"

static const char *TAG = "babyphone_main";




void app_main(void)
{
    ESP_LOGI(TAG, "Babyphone Sender Firmware Starting...");
    // Initialize Hardware FIRST to turn off the backlight and save power
    // Otherwise, transmitting Wi-Fi with the backlight on causes a hardware brownout!
    ESP_ERROR_CHECK(hw_codec_init());
    ESP_ERROR_CHECK(hw_display_init());
    
    // Initialize UI right away
    ESP_ERROR_CHECK(ui_init());

    // Allocate ringbuffers and start audio pipelines BEFORE Wi-Fi
    // This ensures the 32KB internal stack for the Opus encoder can be allocated
    // before the Wi-Fi driver fragments the internal heap!
    RingbufHandle_t pcm_ringbuf = xRingbufferCreate(16384, RINGBUF_TYPE_NOSPLIT);
    RingbufHandle_t rtp_ringbuf = xRingbufferCreate(16384, RINGBUF_TYPE_NOSPLIT);

    ESP_ERROR_CHECK(audio_encoder_init(pcm_ringbuf, rtp_ringbuf));
    ESP_ERROR_CHECK(audio_capture_init(pcm_ringbuf));

    // Initialize Wi-Fi (loads from NVS or launches Captive Portal)
    wifi_manager_start();
    
    // network_tx creates a socket, so it must be initialized AFTER Wi-Fi (LwIP) is up
    ESP_ERROR_CHECK(network_tx_init(rtp_ringbuf));

    ui_set_streaming_status(true);

    // Keep the task alive but stop blinking
    while (1) {
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}
