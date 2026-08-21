#include "audio_encoder.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "audio_encoder";

#define SAMPLE_RATE 8000
#define FRAME_SAMPLES 160 // 20ms at 8kHz

static RingbufHandle_t pcm_in;
static RingbufHandle_t rtp_out;

// G.711a encoding table and function
#define SIGN_BIT (0x80)
#define QUANT_MASK (0xf)
#define NSEGS (8)
#define SEG_SHIFT (4)
#define SEG_MASK (0x70)

static int16_t seg_end[8] = {0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF};

static int search(int16_t val, int16_t *table, int size) {
    for (int i = 0; i < size; i++) {
        if (val <= table[i]) return i;
    }
    return size;
}

static uint8_t linear2alaw(int16_t pcm_val) {
    int mask;
    int seg;
    uint8_t aval;
    if (pcm_val >= 0) {
        mask = 0xD5; // sign (7th) bit = 1
    } else {
        mask = 0x55; // sign bit = 0
        pcm_val = -pcm_val - 8;
    }
    seg = search(pcm_val, seg_end, 8);
    if (seg >= 8) return (0x7F ^ mask);
    else {
        aval = seg << SEG_SHIFT;
        if (seg < 2)
            aval |= (pcm_val >> 4) & QUANT_MASK;
        else
            aval |= (pcm_val >> (seg + 3)) & QUANT_MASK;
        return (aval ^ mask);
    }
}

static void audio_encoder_task(void *args)
{
    ESP_LOGI(TAG, "Audio encoder task started on core %d (G.711a)", xPortGetCoreID());

    uint8_t enc_buffer[12 + FRAME_SAMPLES]; // 12 bytes RTP header + 160 bytes G.711a
    uint32_t rtp_timestamp = 0;
    uint16_t rtp_sequence = 0;

    while (1) {
        size_t item_size;
        int16_t *pcm_frame = (int16_t *)xRingbufferReceive(pcm_in, &item_size, portMAX_DELAY);
        
        if (pcm_frame) {
            // Encode the 20ms frame to G.711a
            for (int i = 0; i < FRAME_SAMPLES; i++) {
                enc_buffer[12 + i] = linear2alaw(pcm_frame[i]);
            }
            vRingbufferReturnItem(pcm_in, pcm_frame);

            // Construct RTP Header (12 bytes)
            enc_buffer[0] = 0x80; // Version 2
            enc_buffer[1] = 8;    // Payload Type (8 for PCMA)
            enc_buffer[2] = rtp_sequence >> 8;
            enc_buffer[3] = rtp_sequence & 0xFF;
            enc_buffer[4] = (rtp_timestamp >> 24) & 0xFF;
            enc_buffer[5] = (rtp_timestamp >> 16) & 0xFF;
            enc_buffer[6] = (rtp_timestamp >> 8) & 0xFF;
            enc_buffer[7] = rtp_timestamp & 0xFF;
            // SSRC (Synchronization Source Identifier) - arbitrary
            enc_buffer[8] = 0x12; enc_buffer[9] = 0x34; enc_buffer[10] = 0x56; enc_buffer[11] = 0x78;

            rtp_sequence++;
            rtp_timestamp += FRAME_SAMPLES;

            // Push RTP packet to network ringbuffer
            xRingbufferSend(rtp_out, enc_buffer, sizeof(enc_buffer), portMAX_DELAY);
        }
    }
}

esp_err_t audio_encoder_init(RingbufHandle_t pcm_in_buf, RingbufHandle_t rtp_out_buf)
{
    pcm_in = pcm_in_buf;
    rtp_out = rtp_out_buf;
    
    // PCMA encoder requires very little stack, pinned to Core 1
    xTaskCreatePinnedToCore(audio_encoder_task, "audio_enc", 4096, NULL, configMAX_PRIORITIES - 2, NULL, 1);
    return ESP_OK;
}
