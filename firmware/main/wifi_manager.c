#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "esp_http_server.h"

#include "wifi_manager.h"
#include "ui.h"

static const char *TAG = "wifi_mgr";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static bool s_provisioning_mode = false;

// Basic URL decode function
static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a'-'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a'-'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16*a+b;
            src+=3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static int s_retry_num = 0;
#define MAX_RETRY 5

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_provisioning_mode) {
            if (s_retry_num < MAX_RETRY) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(TAG, "Retrying Wi-Fi connection...");
            } else {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0; // Reset retry counter on success
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ui_set_ip_address(ip_str);
        ui_set_wifi_status(true);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void dns_server_task(void *pvParameters) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    
    char rx_buffer[128];
    char tx_buffer[128];
    while(1) {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &addr_len);
        if (len > 12) {
            memcpy(tx_buffer, rx_buffer, len);
            tx_buffer[2] = 0x81;
            tx_buffer[3] = 0x80;
            tx_buffer[6] = 0x00;
            tx_buffer[7] = 0x01;
            tx_buffer[8] = 0x00;
            tx_buffer[9] = 0x00;
            tx_buffer[10] = 0x00;
            tx_buffer[11] = 0x00;
            
            int ptr = len;
            tx_buffer[ptr++] = 0xc0;
            tx_buffer[ptr++] = 0x0c;
            tx_buffer[ptr++] = 0x00;
            tx_buffer[ptr++] = 0x01;
            tx_buffer[ptr++] = 0x00;
            tx_buffer[ptr++] = 0x01;
            tx_buffer[ptr++] = 0x00;
            tx_buffer[ptr++] = 0x00;
            tx_buffer[ptr++] = 0x00;
            tx_buffer[ptr++] = 0x00;
            tx_buffer[ptr++] = 0x00;
            tx_buffer[ptr++] = 0x04;
            tx_buffer[ptr++] = 192;
            tx_buffer[ptr++] = 168;
            tx_buffer[ptr++] = 4;
            tx_buffer[ptr++] = 1;
            
            sendto(sock, tx_buffer, ptr, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
        }
    }
}

static const char* index_html = 
    "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>"
    "<style>body{font-family:sans-serif;padding:20px;background:#f0f0f0;} "
    ".container{background:#fff;padding:20px;border-radius:10px;box-shadow:0 0 10px rgba(0,0,0,0.1);max-width:400px;margin:auto;}"
    "input[type='text'],input[type='password']{width:100%;padding:10px;margin:10px 0 5px 0;border:1px solid #ccc;border-radius:5px;box-sizing:border-box;}"
    "input[type='submit']{width:100%;padding:10px;background:#00bcd4;color:#fff;border:none;border-radius:5px;font-size:16px;cursor:pointer;}"
    ".cb-container{font-size:14px; color:#555; margin-bottom:15px;}</style>"
    "<script>function togglePw(){var x=document.getElementById('pw');if(x.type==='password'){x.type='text';}else{x.type='password';}}</script>"
    "</head>"
    "<body><div class='container'><h2>Babyphone Setup</h2>"
    "<form action='/setup' method='POST'>"
    "SSID:<br><input type='text' name='ssid'><br>"
    "Password:<br><input type='password' id='pw' name='pass'>"
    "<div class='cb-container'><input type='checkbox' onclick='togglePw()'> Show Password</div>"
    "<input type='submit' value='Save & Reboot'>"
    "</form></div></body></html>";

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t setup_post_handler(httpd_req_t *req) {
    char buf[128];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char param_ssid[64] = {0};
    char param_pass[64] = {0};
    
    char dec_ssid[64] = {0};
    char dec_pass[64] = {0};

    if (httpd_query_key_value(buf, "ssid", param_ssid, sizeof(param_ssid)) == ESP_OK &&
        httpd_query_key_value(buf, "pass", param_pass, sizeof(param_pass)) == ESP_OK) {
        
        url_decode(dec_ssid, param_ssid);
        url_decode(dec_pass, param_pass);

        nvs_handle_t my_handle;
        if (nvs_open("wifi", NVS_READWRITE, &my_handle) == ESP_OK) {
            nvs_set_str(my_handle, "ssid", dec_ssid);
            nvs_set_str(my_handle, "pass", dec_pass);
            nvs_commit(my_handle);
            nvs_close(my_handle);
        }

        const char* success_html = "<html><body><h2>Saved! Rebooting...</h2></body></html>";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, success_html, HTTPD_RESP_USE_STRLEN);

        ESP_LOGI(TAG, "Credentials saved, rebooting in 2 seconds...");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        httpd_resp_send_500(req);
    }
    return ESP_OK;
}

static esp_err_t captive_portal_handler(httpd_req_t *req) {
    // Redirect all 404 requests to the root index
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

void wifi_manager_start(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    nvs_handle_t my_handle;
    char ssid[64] = {0};
    char pass[64] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(pass);
    bool has_creds = false;

        if (nvs_open("wifi", NVS_READONLY, &my_handle) == ESP_OK) {
        if (nvs_get_str(my_handle, "ssid", ssid, &ssid_len) == ESP_OK &&
            nvs_get_str(my_handle, "pass", pass, &pass_len) == ESP_OK) {
            has_creds = true;
        }
        nvs_close(my_handle);
    }

    if (has_creds) {
        ESP_LOGI(TAG, "Trying saved Wi-Fi: %s", ssid);
        wifi_config_t wifi_config = {0};
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
        
        // Improve compatibility for modern routers (WPA2/WPA3 mixed, Band Steering)
        wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, 20000 / portTICK_PERIOD_MS);

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Successfully connected to saved Wi-Fi");
            ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
            return;
        }
        ESP_LOGE(TAG, "Failed to connect to saved Wi-Fi. Falling back to Setup Mode.");
    } else {
        ESP_LOGI(TAG, "No saved Wi-Fi credentials found. Entering Setup Mode.");
    }

    // Enter SoftAP Provisioning Mode
    s_provisioning_mode = true;
    esp_netif_create_default_wifi_ap();
    
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "Babyphone-Setup",
            .ssid_len = strlen("Babyphone-Setup"),
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    
    if (!has_creds) {
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    ui_set_provisioning_mode();

    // Start DNS Server Task
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL);

    // Start HTTP Server
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.max_open_sockets = 7;
    server_config.lru_purge_enable = true;
    
    // Captive portals require redirecting 404s
    server_config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &server_config) == ESP_OK) {
        httpd_uri_t uri_get = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_get);
        
        httpd_uri_t uri_post = { .uri = "/setup", .method = HTTP_POST, .handler = setup_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_post);
        
        // Catch-all for captive portal (Apple devices request /hotspot-detect.html, etc)
        httpd_uri_t uri_catchall = { .uri = "/*", .method = HTTP_GET, .handler = captive_portal_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_catchall);
    }

    // Keep the task alive in AP mode indefinitely
    while(1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
