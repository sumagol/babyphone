#include "audio_encoder.h"
#include "esp_log.h"
#include <string.h>
#include "opus.h"
#include "sw_i2c.h"
#include "mbedtls/aes.h"
#include "ui.h"

static const char *TAG = "audio_encoder";

#define SAMPLE_RATE 48000
#define FRAME_SAMPLES 960 // 20ms at 48kHz
#define OPUS_BITRATE 24000 // 24 kbps
#define MAX_PAYLOAD_BYTES 128 // Opus frames at 24kbps are typically small (~60 bytes)

static RingbufHandle_t pcm_in = NULL;
static RingbufHandle_t rtp_out = NULL;

static void audio_encoder_task(void *args)
{
    ESP_LOGI(TAG, "Audio encoder task started on core %d (Opus)", xPortGetCoreID());

    // Initialize AES-128-CTR with static key
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    const unsigned char key[16] = "BabyPhoneKey2026"; // Exactly 16 bytes
    mbedtls_aes_setkey_enc(&aes, key, 128);

    int err;
    OpusEncoder *encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || encoder == NULL) {
        ESP_LOGE(TAG, "Failed to create Opus encoder: %d", err);
        vTaskDelete(NULL);
    }
    
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
    // CRITICAL: M5StickS3 Opus encode takes 100% CPU without complexity=0 and -O3, starving Wi-Fi!
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));

    uint8_t enc_buffer[12 + 8 + MAX_PAYLOAD_BYTES]; // 12 bytes RTP header + 8 bytes extension + Opus payload
    uint32_t rtp_timestamp = 0;
    uint16_t rtp_sequence = 0;
    
    uint8_t real_battery = 100;
    bool real_charging = false;
    uint32_t frame_count = 0;
    
    // Initialize Software I2C on pins 47 (SDA) and 48 (SCL)
    sw_i2c_init(47, 48);

    while (1) {
        size_t item_size;
        int16_t *pcm_frame = (int16_t *)xRingbufferReceive(pcm_in, &item_size, portMAX_DELAY);
        
        if (pcm_frame) {
            frame_count++;
            
            // Read real PMIC values every 5 seconds (250 frames)
            if (frame_count % 250 == 0) {
                uint8_t bat_l = 0, bat_h = 0;
                uint8_t pwr_src = 2; // Default to battery
                
                // Read from M5PM1 (0x6E)
                if (sw_i2c_read_reg(0x6E, 0x22, &bat_l) && sw_i2c_read_reg(0x6E, 0x23, &bat_h)) {
                    uint16_t voltage_mv = (bat_h << 8) | bat_l;
                    static float filtered_voltage = -1;
                    if (filtered_voltage < 0) {
                        filtered_voltage = voltage_mv; // Initialize on first read
                    } else {
                        filtered_voltage = (0.8 * filtered_voltage) + (0.2 * voltage_mv); // Low-pass filter
                    }
                    
                    // Convert filtered voltage (3300mV - 4200mV) to percentage (0 - 100)
                    if (filtered_voltage >= 4150) real_battery = 100;
                    else if (filtered_voltage <= 3300) real_battery = 0;
                    else real_battery = (uint8_t)(((filtered_voltage - 3300) * 100) / (4150 - 3300));
                    
                    uint8_t vin_l = 0, vin_h = 0;
                    if (sw_i2c_read_reg(0x6E, 0x24, &vin_l) && sw_i2c_read_reg(0x6E, 0x25, &vin_h)) {
                        uint16_t vin_mv = (vin_h << 8) | vin_l;
                        real_charging = (vin_mv > 4000); // 5V USB rail > 4V means it is plugged in!
                    }
                }
                // Fallback to AXP2101 (0x34) if M5PM1 is not responding
                else if (sw_i2c_read_reg(0x34, 0xA4, &bat_l)) {
                    real_battery = bat_l & 0x7F;
                    uint8_t chg_val = 0;
                    if (sw_i2c_read_reg(0x34, 0x01, &chg_val)) {
                        uint8_t status = chg_val & 0x07;
                        real_charging = (status == 1 || status == 2 || status == 3);
                    }
                }
                
                // Update UI Battery Level
                ui_set_battery_level(real_battery);
            }

            // Encode the 20ms frame with Opus. Offset by 20 bytes (12 standard + 8 extension)
            int nbBytes = opus_encode(encoder, pcm_frame, FRAME_SAMPLES, enc_buffer + 20, MAX_PAYLOAD_BYTES);
            
            vRingbufferReturnItem(pcm_in, pcm_frame);

            if (nbBytes > 0) {
                // Construct RTP Header (12 bytes)
                enc_buffer[0] = 0x90; // Version 2 (0x80) | Extension bit set (0x10)
                enc_buffer[1] = 96;   // Payload Type (96 for Dynamic/Opus)
                enc_buffer[2] = rtp_sequence >> 8;
                enc_buffer[3] = rtp_sequence & 0xFF;
                enc_buffer[4] = (rtp_timestamp >> 24) & 0xFF;
                enc_buffer[5] = (rtp_timestamp >> 16) & 0xFF;
                enc_buffer[6] = (rtp_timestamp >> 8) & 0xFF;
                enc_buffer[7] = rtp_timestamp & 0xFF;
                // SSRC (Synchronization Source Identifier) - arbitrary
                enc_buffer[8] = 0x12; enc_buffer[9] = 0x34; enc_buffer[10] = 0x56; enc_buffer[11] = 0x78;

                // RTP Extension Header (4 bytes)
                enc_buffer[12] = 0xBA; // Profile ID: 0xBABB ("Baby")
                enc_buffer[13] = 0xBB;
                enc_buffer[14] = 0x00; // Length: 1 (meaning one 32-bit word follows)
                enc_buffer[15] = 0x01;

                // RTP Extension Payload (4 bytes)
                // Pack Charging State (Bit 7) and Battery Level (Bits 0-6)
                uint8_t telemetry = (real_battery & 0x7F) | (real_charging ? 0x80 : 0x00);
                
                enc_buffer[16] = telemetry;
                enc_buffer[17] = 0x00;
                enc_buffer[18] = 0x00;
                enc_buffer[19] = 0x00;

                rtp_sequence++;
                // RFC 7587: Opus RTP timestamp MUST increment at 48000 Hz regardless of sample rate!
                // 20ms frame = 48000 * 0.02 = 960.
                rtp_timestamp += 960;

                // --- AES-128-CTR ENCRYPTION ---
                unsigned char iv[16] = {0};
                // IV = SSRC (4) + Sequence (2) + Timestamp (4) + Padding (6)
                iv[0] = enc_buffer[8];  iv[1] = enc_buffer[9];  iv[2] = enc_buffer[10]; iv[3] = enc_buffer[11];
                iv[4] = enc_buffer[2];  iv[5] = enc_buffer[3];
                iv[6] = enc_buffer[4];  iv[7] = enc_buffer[5];  iv[8] = enc_buffer[6];  iv[9] = enc_buffer[7];
                
                size_t nc_off = 0;
                unsigned char stream_block[16] = {0};
                mbedtls_aes_crypt_ctr(&aes, nbBytes, &nc_off, iv, stream_block, enc_buffer + 20, enc_buffer + 20);

                // Push RTP packet to network ringbuffer (20 bytes headers + Opus payload)
                xRingbufferSend(rtp_out, enc_buffer, 20 + nbBytes, portMAX_DELAY);
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
