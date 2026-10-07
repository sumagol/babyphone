#include "audio_capture.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <math.h>

static const char *TAG = "audio_capture";

#define I2S_MCLK 18
#define I2S_BCLK 17
#define I2S_SAMPLE_RATE 48000
#define I2S_LRCK 15
#define I2S_SDIN 16
#define I2S_SDOUT 14

#define SAMPLE_RATE 48000
#define FRAME_SAMPLES 960 
// 20ms frame at 48kHz = 960 samples. 16-bit mono = 2 bytes per sample = 1920 bytes per frame.
#define FRAME_SIZE_BYTES (FRAME_SAMPLES * 2)

static i2s_chan_handle_t rx_chan;
static RingbufHandle_t pcm_out;
static noise_guard_mode_t current_guard_mode = NOISE_GUARD_0_5S;

#include "ui.h"

const char* audio_capture_get_noise_guard_str(noise_guard_mode_t mode)
{
    switch (mode) {
        case NOISE_GUARD_0_5S:  return "0.5s";
        case NOISE_GUARD_5_0S:  return "5.0s";
        case NOISE_GUARD_10_0S: return "10.0s";
        default:                return "0.5s";
    }
}

noise_guard_mode_t audio_capture_get_noise_guard(void)
{
    return current_guard_mode;
}

void audio_capture_set_noise_guard(noise_guard_mode_t mode)
{
    if (mode >= NOISE_GUARD_MAX) mode = NOISE_GUARD_0_5S;
    current_guard_mode = mode;

    nvs_handle_t h;
    if (nvs_open("settings", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "guard_mode", (uint8_t)mode);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Noise guard mode set to: %s", audio_capture_get_noise_guard_str(mode));
}

static inline int get_required_noise_frames(noise_guard_mode_t mode)
{
    switch (mode) {
        case NOISE_GUARD_5_0S:  return 250; // 250 * 20ms = 5.0 seconds
        case NOISE_GUARD_10_0S: return 500; // 500 * 20ms = 10.0 seconds
        case NOISE_GUARD_0_5S:
        default:                return 25;  // 25 * 20ms = 0.5 seconds
    }
}

static void audio_capture_task(void *args)
{
    ESP_LOGI(TAG, "Audio capture task started on core %d", xPortGetCoreID());
    
    int16_t *rx_buf = (int16_t *)malloc(FRAME_SIZE_BYTES);
    if (!rx_buf) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        vTaskDelete(NULL);
    }

    size_t bytes_read = 0;
    int frame_count = 0;
    
    int noise_consecutive_frames = 0;
    int silence_consecutive_frames = 0;
    bool gate_open = false;
    const int HANGOVER_FRAMES = 125; // 125 * 20ms = 2.5s hold time after noise drops
    
    while (1) {
        esp_err_t ret = i2s_channel_read(rx_chan, rx_buf, FRAME_SIZE_BYTES, &bytes_read, portMAX_DELAY);
        if (ret == ESP_OK && bytes_read == FRAME_SIZE_BYTES) {
            
            // Calculate RMS for every frame to use for both the VU meter and the Noise Gate
            double sum_sq = 0.0;
            for (int i = 0; i < FRAME_SAMPLES; i++) {
                double sample = (double)rx_buf[i];
                sum_sq += sample * sample;
            }
            double rms = sqrt(sum_sq / FRAME_SAMPLES);

            // --- SOFTWARE NOISE GUARD DURATION LOGIC ---
            // Threshold tuned for baby sounds above low-level mic hiss
            const double NOISE_GATE_THRESHOLD = 75.0;

            if (rms >= NOISE_GATE_THRESHOLD) {
                noise_consecutive_frames++;
                silence_consecutive_frames = 0;
                int req = get_required_noise_frames(current_guard_mode);
                if (noise_consecutive_frames >= req) {
                    gate_open = true;
                }
            } else {
                noise_consecutive_frames = 0;
                if (gate_open) {
                    silence_consecutive_frames++;
                    if (silence_consecutive_frames >= HANGOVER_FRAMES) {
                        gate_open = false;
                        silence_consecutive_frames = 0;
                    }
                }
            }

            if (gate_open) {
                // Gate is open: baby noise is active.
                // Apply subtle soft gate on quieter breathing/pauses during hangover to suppress hiss
                if (rms < NOISE_GATE_THRESHOLD) {
                    double factor = (rms / NOISE_GATE_THRESHOLD);
                    factor = factor * factor; 
                    for (int i = 0; i < FRAME_SAMPLES; i++) {
                        rx_buf[i] = (int16_t)(rx_buf[i] * factor);
                    }
                }
            } else {
                // Gate is closed: silence frame to avoid room clicks/single coughs,
                // while continuing to push silent frames to keep RTP alive and prevent client beep.
                memset(rx_buf, 0, FRAME_SIZE_BYTES);
            }

            // Push the 20ms frame to the encoder via Ringbuffer
            xRingbufferSend(pcm_out, rx_buf, FRAME_SIZE_BYTES, portMAX_DELAY);
            
            // Update UI
            if (++frame_count >= 5) { // every 100ms
                frame_count = 0;
                
                // Calculate dB for VU meter (reflects actual mic level)
                double ref = 32768.0; 
                double db = (rms > 1.0) ? 20.0 * log10(rms / ref) : -60.0;
                ui_set_audio_level((int)db);

                // Update Smart Sleep State based on gate state
                ui_set_smart_sleep_state(gate_open);
            }
            
        } else {
            ESP_LOGE(TAG, "I2S read failed: %d, bytes_read: %zu", ret, bytes_read);
            vTaskDelay(pdMS_TO_TICKS(10)); // Prevent watchdog/starvation if I2S is returning immediately
        }
    }
}

esp_err_t audio_capture_init(RingbufHandle_t pcm_out_buf)
{
    pcm_out = pcm_out_buf;

    // Load saved guard mode from NVS
    nvs_handle_t h;
    uint8_t saved_mode = 0;
    if (nvs_open("settings", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "guard_mode", &saved_mode) == ESP_OK) {
            if (saved_mode < NOISE_GUARD_MAX) {
                current_guard_mode = (noise_guard_mode_t)saved_mode;
            }
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Loaded noise guard mode: %s", audio_capture_get_noise_guard_str(current_guard_mode));

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
    // Force DMA to capture only ONE slot to prevent reading stereo
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ret = i2s_channel_init_std_mode(rx_chan, &std_cfg);
    if (ret != ESP_OK) return ret;

    ret = i2s_channel_enable(rx_chan);
    if (ret != ESP_OK) return ret;

    // Create FreeRTOS task pinned to Core 0 with High Priority (configMAX_PRIORITIES - 1)
    xTaskCreatePinnedToCore(audio_capture_task, "audio_capture", 4096, NULL, configMAX_PRIORITIES - 1, NULL, 0);

    return ESP_OK;
}
