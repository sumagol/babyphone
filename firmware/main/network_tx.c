#include "network_tx.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "network_tx";

#define MULTICAST_IP "239.255.0.1"
#define MULTICAST_PORT 5004

static RingbufHandle_t rtp_in;

static void network_tx_task(void *args)
{
    ESP_LOGI(TAG, "Network TX task started on core %d", xPortGetCoreID());

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
    }

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(MULTICAST_PORT);
    dest_addr.sin_addr.s_addr = inet_addr(MULTICAST_IP);

    // Set TTL to 1 (local subnet only)
    uint8_t ttl = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(uint8_t));

    ESP_LOGI(TAG, "Socket created, streaming to %s:%d", MULTICAST_IP, MULTICAST_PORT);

    while (1) {
        size_t item_size;
        uint8_t *rtp_packet = (uint8_t *)xRingbufferReceive(rtp_in, &item_size, portMAX_DELAY);
        
        if (rtp_packet) {
            ESP_LOGI(TAG, "Received packet from ringbuf, size: %d, first byte: 0x%02x", (int)item_size, rtp_packet[0]);
            
            int err = sendto(sock, rtp_packet, item_size, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            if (err < 0) {
                ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            } else {
                ESP_LOGI(TAG, "Packet sent successfully");
            }
            vRingbufferReturnItem(rtp_in, rtp_packet);
        }
    }
}

esp_err_t network_tx_init(RingbufHandle_t rtp_in_buf)
{
    rtp_in = rtp_in_buf;
    
    // Pinned to Core 0 (alongside audio capture)
    xTaskCreatePinnedToCore(network_tx_task, "network_tx", 16384, NULL, configMAX_PRIORITIES - 2, NULL, 0);
    return ESP_OK;
}
