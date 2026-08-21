#include "audio_capture.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char *TAG = "audio_capture";

#define I2S_MCLK 18
#define I2S_BCLK 17
#define I2S_SAMPLE_RATE 8000
#define I2S_LRCK 15
#define I2S_SDIN 16
#define I2S_SDOUT 14

#define SAMPLE_RATE 8000
#define FRAME_SAMPLES 160 
// We will capture BOTH slots (stereo) to avoid missing the active channel.
// 16-bit stereo = 4 bytes per sample frame.
#define FRAME_SIZE_BYTES (FRAME_SAMPLES * 4)

static i2s_chan_handle_t rx_chan;

static RingbufHandle_t pcm_out;

static void audio_capture_task(void *args)
{
    ESP_LOGI(TAG, "Audio capture task started on core %d", xPortGetCoreID());
    
    int16_t *rx_buf = (int16_t *)malloc(FRAME_SIZE_BYTES);
    if (!rx_buf) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        vTaskDelete(NULL);
    }

    size_t bytes_read = 0;
    while (1) {
        esp_err_t ret = i2s_channel_read(rx_chan, rx_buf, FRAME_SIZE_BYTES, &bytes_read, portMAX_DELAY);
        if (ret == ESP_OK && bytes_read == FRAME_SIZE_BYTES) {
            // Push the stereo frame (640 bytes) to the encoder via Ringbuffer
            xRingbufferSend(pcm_out, rx_buf, FRAME_SIZE_BYTES, portMAX_DELAY);
        } else {
            ESP_LOGE(TAG, "I2S read failed: %d, bytes_read: %d", ret, bytes_read);
            vTaskDelay(pdMS_TO_TICKS(10)); // Prevent watchdog/starvation if I2S is returning immediately
        }
    }
}

esp_err_t audio_capture_init(RingbufHandle_t pcm_out_buf)
{
    pcm_out = pcm_out_buf;
    ESP_LOGI(TAG, "Initializing I2S RX channel...");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &rx_chan);
    if (ret != ESP_OK) return ret;

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws   = I2S_LRCK,
            .dout = I2S_SDOUT,
            .din  = I2S_SDIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    // Force standard MCLK multiplier
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    // Capture both slots so we don't miss the mic data
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    ret = i2s_channel_init_std_mode(rx_chan, &std_cfg);
    if (ret != ESP_OK) return ret;

    ret = i2s_channel_enable(rx_chan);
    if (ret != ESP_OK) return ret;

    // Create FreeRTOS task pinned to Core 0 with High Priority (configMAX_PRIORITIES - 1)
    xTaskCreatePinnedToCore(audio_capture_task, "audio_capture", 4096, NULL, configMAX_PRIORITIES - 1, NULL, 0);

    return ESP_OK;
}
