idf_component_register(SRCS "main.c" "hw_codec.c" "hw_display.c" "ui.c" "audio_capture.c" "audio_encoder.c" "network_tx.c" "sw_i2c.c" "wifi_manager.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd esp_wifi nvs_flash esp_netif esp_event lwip esp_http_server mbedtls)
