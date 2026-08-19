idf_component_register(SRCS "main.c" "hw_codec.c" "hw_display.c" "audio_capture.c" "audio_encoder.c" "network_tx.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd esp_wifi nvs_flash esp_netif esp_event lwip)
