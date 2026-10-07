#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NOISE_GUARD_0_5S = 0,
    NOISE_GUARD_5_0S = 1,
    NOISE_GUARD_10_0S = 2,
    NOISE_GUARD_MAX
} noise_guard_mode_t;

/**
 * @brief Initialize the I2S hardware interface for reading from the ES8311 codec.
 * 
 * @param pcm_out_buf Ringbuffer handle to write raw PCM frames to (output).
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t audio_capture_init(RingbufHandle_t pcm_out_buf);

/**
 * @brief Set the noise guard duration threshold and persist to NVS
 * @param mode The selected mode (0: 0.5s, 1: 5.0s, 2: 10.0s)
 */
void audio_capture_set_noise_guard(noise_guard_mode_t mode);

/**
 * @brief Get the current noise guard mode
 */
noise_guard_mode_t audio_capture_get_noise_guard(void);

/**
 * @brief Get user-readable text for a noise guard mode
 */
const char* audio_capture_get_noise_guard_str(noise_guard_mode_t mode);

#ifdef __cplusplus
}
#endif
