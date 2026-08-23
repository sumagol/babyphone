#include "audio_encoder.h"
#include "esp_log.h"
#include <string.h>
#include "opus.h"

static const char *TAG = "audio_encoder";

#define SAMPLE_RATE 48000
#define FRAME_SAMPLES 960 // 20ms at 48kHz
#define OPUS_BITRATE 24000 // 24 kbps
#define MAX_PAYLOAD_BYTES 128 // Opus frames at 24kbps are typically small (~60 bytes)

static RingbufHandle_t pcm_in;
static RingbufHandle_t rtp_out;

static void audio_encoder_task(void *args)
{
    ESP_LOGI(TAG, "Audio encoder task started on core %d (Opus)", xPortGetCoreID());

    int err;
    OpusEncoder *encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || encoder == NULL) {
        ESP_LOGE(TAG, "Failed to create Opus encoder: %d", err);
        vTaskDelete(NULL);
    }
    
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
    // CRITICAL: M5StickS3 Opus encode takes 100% CPU without complexity=0 and -O3, starving Wi-Fi!
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));

    uint8_t enc_buffer[12 + MAX_PAYLOAD_BYTES]; // 12 bytes RTP header + Opus payload
    uint32_t rtp_timestamp = 0;
    uint16_t rtp_sequence = 0;

    while (1) {
        size_t item_size;
        int16_t *pcm_frame = (int16_t *)xRingbufferReceive(pcm_in, &item_size, portMAX_DELAY);
        
        if (pcm_frame) {
            // Encode the 20ms frame with Opus
            int nbBytes = opus_encode(encoder, pcm_frame, FRAME_SAMPLES, enc_buffer + 12, MAX_PAYLOAD_BYTES);
            
            vRingbufferReturnItem(pcm_in, pcm_frame);

            if (nbBytes > 0) {
                // Construct RTP Header (12 bytes)
                enc_buffer[0] = 0x80; // Version 2
                enc_buffer[1] = 96;   // Payload Type (96 for Dynamic/Opus)
                enc_buffer[2] = rtp_sequence >> 8;
                enc_buffer[3] = rtp_sequence & 0xFF;
                enc_buffer[4] = (rtp_timestamp >> 24) & 0xFF;
                enc_buffer[5] = (rtp_timestamp >> 16) & 0xFF;
                enc_buffer[6] = (rtp_timestamp >> 8) & 0xFF;
                enc_buffer[7] = rtp_timestamp & 0xFF;
                // SSRC (Synchronization Source Identifier) - arbitrary
                enc_buffer[8] = 0x12; enc_buffer[9] = 0x34; enc_buffer[10] = 0x56; enc_buffer[11] = 0x78;

                rtp_sequence++;
                // RFC 7587: Opus RTP timestamp MUST increment at 48000 Hz regardless of sample rate!
                // 20ms frame = 48000 * 0.02 = 960.
                rtp_timestamp += 960;

                // Push RTP packet to network ringbuffer
                xRingbufferSend(rtp_out, enc_buffer, 12 + nbBytes, portMAX_DELAY);
            } else {
                ESP_LOGE(TAG, "Opus encode failed: %d", nbBytes);
            }
        }
    }
    
    opus_encoder_destroy(encoder);
    vTaskDelete(NULL);
}

esp_err_t audio_encoder_init(RingbufHandle_t pcm_in_buf, RingbufHandle_t rtp_out_buf)
{
    pcm_in = pcm_in_buf;
    rtp_out = rtp_out_buf;
    
    ESP_LOGI(TAG, "Free internal heap before task: %d bytes", (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Largest internal free block: %d bytes", (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    // Pin to Core 0 (alongside Wi-Fi/Audio capture) to leave Core 1 free for LVGL!
    BaseType_t ret = xTaskCreatePinnedToCore(audio_encoder_task, "audio_enc", 32768, NULL, configMAX_PRIORITIES - 2, NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio_enc task! (Out of memory?)");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
