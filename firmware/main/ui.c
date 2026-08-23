#include "ui.h"
#include "hw_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "lvgl.h"

#define BTN_PIN 11
#define TIMEOUT_MS 60000

static const char *TAG = "ui";

static SemaphoreHandle_t xGuiSemaphore;
static lv_disp_draw_buf_t disp_buf;
static lv_color_t *buf1;

// UI Elements
static lv_obj_t * tv;
static lv_obj_t * label_status;
static lv_obj_t * label_ip;

// State
static uint32_t last_activity_time = 0;
static bool is_screen_off = false;
static int current_page = 0;
#define TOTAL_PAGES 3

static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s)
static lv_disp_drv_t disp_drv;      // contains callback functions

// Global UI handles
static lv_obj_t * label_status;
static lv_obj_t * label_rssi;
static lv_obj_t * label_uptime;
static lv_obj_t * bar_vu;
static lv_obj_t * label_db;

static void my_flush_ready_cb(void) {
    lv_disp_flush_ready(&disp_drv);
}

static void disp_flush(lv_disp_drv_t * disp_drv_ptr, const lv_area_t * area, lv_color_t * color_p)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) disp_drv_ptr->user_data;
    
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;

    // LVGL is now configured with CONFIG_LV_COLOR_16_SWAP=y
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_p);
}

static void increase_lvgl_tick(void *arg)
{
    lv_tick_inc(5);
}

static void handle_button_press(void)
{
    last_activity_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    if (is_screen_off) {
        ESP_LOGI(TAG, "Waking up screen");
        hw_display_set_backlight(50);
        is_screen_off = false;
    } else {
        current_page = (current_page + 1) % TOTAL_PAGES;
        if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
            lv_obj_set_tile_id(tv, 0, current_page, LV_ANIM_ON);
            xSemaphoreGive(xGuiSemaphore);
        }
    }
}

#include "esp_wifi.h"

static void gui_task(void *arg)
{
    ESP_LOGI(TAG, "Starting GUI task");
    
    gpio_config_t btn_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BTN_PIN),
        .pull_down_en = 0,
        .pull_up_en = 1
    };
    gpio_config(&btn_conf);

    last_activity_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int last_btn_state = 1;
    int tick_count = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        tick_count++;
        
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (!is_screen_off && (current_time - last_activity_time > TIMEOUT_MS)) {
            ESP_LOGI(TAG, "Screen timeout, turning off backlight");
            hw_display_set_backlight(0);
            is_screen_off = true;
        }

        int btn_state = gpio_get_level(BTN_PIN);
        if (btn_state == 0 && last_btn_state == 1) {
            vTaskDelay(pdMS_TO_TICKS(50)); 
            if (gpio_get_level(BTN_PIN) == 0) {
                handle_button_press();
            }
        }
        last_btn_state = btn_state;

        // Poll RSSI every 2 seconds (200 * 10ms)
        if (tick_count >= 200) {
            tick_count = 0;
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                ui_set_rssi(ap_info.rssi);
            }
            
            // Update uptime (every 2 seconds is fine)
            if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
                uint32_t uptime_sec = esp_timer_get_time() / 1000000;
                uint32_t hours = uptime_sec / 3600;
                uint32_t mins = (uptime_sec % 3600) / 60;
                uint32_t secs = uptime_sec % 60;
                lv_label_set_text_fmt(label_uptime, "UPTIME: %02luh:%02lum:%02lus", hours, mins, secs);
                xSemaphoreGive(xGuiSemaphore);
            }
        }

        if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(xGuiSemaphore);
        }
    }
}

static void build_ui(void)
{
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN);

    tv = lv_tileview_create(lv_scr_act());
    lv_obj_set_style_bg_color(tv, lv_color_hex(0x000000), LV_PART_MAIN);

    // --- Page 0: Main Status ---
    lv_obj_t * tile1 = lv_tileview_add_tile(tv, 0, 0, LV_DIR_BOTTOM);
    
    lv_obj_t * label_title = lv_label_create(tile1);
    lv_label_set_text(label_title, "BABYPHONE");
    lv_obj_set_style_text_color(label_title, lv_color_hex(0x00FFFF), LV_PART_MAIN); // Cyan title
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 20);

    label_status = lv_label_create(tile1);
    lv_label_set_long_mode(label_status, LV_LABEL_LONG_SCROLL_CIRCULAR); 
    lv_obj_set_width(label_status, 130);
    lv_obj_set_style_text_align(label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label_status, "Connecting...");
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(label_status, LV_ALIGN_CENTER, 0, -20);

    label_rssi = lv_label_create(tile1);
    lv_label_set_text(label_rssi, "WiFi: -- dBm");
    lv_obj_set_style_text_color(label_rssi, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_align(label_rssi, LV_ALIGN_CENTER, 0, 10);

    label_uptime = lv_label_create(tile1);
    lv_label_set_text(label_uptime, "UPTIME: 00h:00m:00s");
    lv_obj_set_style_text_color(label_uptime, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_align(label_uptime, LV_ALIGN_BOTTOM_MID, 0, -20);

    // --- Page 1: Network & Info ---
    lv_obj_t * tile2 = lv_tileview_add_tile(tv, 0, 1, LV_DIR_TOP | LV_DIR_BOTTOM);
    
    lv_obj_t * label_net_title = lv_label_create(tile2);
    lv_label_set_text(label_net_title, "TARGET IP");
    lv_obj_set_style_text_color(label_net_title, lv_color_hex(0x00FFFF), LV_PART_MAIN);
    lv_obj_align(label_net_title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t * label_mcast = lv_label_create(tile2);
    lv_label_set_text(label_mcast, "239.255.0.1\nPort 5004");
    lv_obj_set_style_text_align(label_mcast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label_mcast, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(label_mcast, LV_ALIGN_CENTER, 0, -20);

    label_ip = lv_label_create(tile2);
    lv_label_set_long_mode(label_ip, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(label_ip, 130);
    lv_obj_set_style_text_align(label_ip, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label_ip, "LOCAL: 0.0.0.0");
    lv_obj_set_style_text_color(label_ip, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_align(label_ip, LV_ALIGN_BOTTOM_MID, 0, -20);

    // --- Page 2: Audio (VU) ---
    lv_obj_t * tile3 = lv_tileview_add_tile(tv, 0, 2, LV_DIR_TOP);

    lv_obj_t * label_audio_title = lv_label_create(tile3);
    lv_label_set_text(label_audio_title, "AUDIO (VU)");
    lv_obj_set_style_text_color(label_audio_title, lv_color_hex(0x00FFFF), LV_PART_MAIN);
    lv_obj_align(label_audio_title, LV_ALIGN_TOP_MID, 0, 20);

    bar_vu = lv_bar_create(tile3);
    lv_obj_set_size(bar_vu, 110, 20);
    lv_obj_align(bar_vu, LV_ALIGN_CENTER, 0, -10);
    lv_bar_set_range(bar_vu, -60, 0); // -60dB to 0dB
    lv_bar_set_value(bar_vu, -60, LV_ANIM_OFF);

    // Style the bar to be green by default
    lv_obj_set_style_bg_color(bar_vu, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_vu, lv_color_hex(0x00FF00), LV_PART_INDICATOR);

    label_db = lv_label_create(tile3);
    lv_label_set_text(label_db, "-60 dB");
    lv_obj_set_style_text_color(label_db, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(label_db, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t * label_codec = lv_label_create(tile3);
    lv_label_set_text(label_codec, "OPUS 24kbps");
    lv_obj_set_style_text_color(label_codec, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_align(label_codec, LV_ALIGN_BOTTOM_MID, 0, -20);
}

esp_err_t ui_init(void)
{
    xGuiSemaphore = xSemaphoreCreateMutex();
    if (!xGuiSemaphore) {
        return ESP_ERR_NO_MEM;
    }

    lv_init();

    size_t draw_buf_sz = 135 * 240 * sizeof(lv_color_t) / 10;
    buf1 = heap_caps_malloc(draw_buf_sz, MALLOC_CAP_DMA);
    if (!buf1) {
        return ESP_ERR_NO_MEM;
    }

    lv_disp_draw_buf_init(&disp_buf, buf1, NULL, 135 * 240 / 10);

    lv_disp_drv_init(&disp_drv);
    
    disp_drv.hor_res = 135;
    disp_drv.ver_res = 240;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = hw_display_get_panel();
    lv_disp_drv_register(&disp_drv);

    hw_display_set_flush_cb(my_flush_ready_cb);

    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "periodic_gui"
    };
    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 5 * 1000));

    if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
        build_ui();
        xSemaphoreGive(xGuiSemaphore);
    }

    xTaskCreatePinnedToCore(gui_task, "gui_task", 4096, NULL, 5, NULL, 1);

    return ESP_OK;
}

void ui_set_wifi_status(bool connected)
{
    if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
        if (connected) {
            lv_label_set_text(label_status, "Wi-Fi Connected");
            lv_obj_set_style_text_color(label_status, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        } else {
            lv_label_set_text(label_status, "Wi-Fi Disconnected");
            lv_obj_set_style_text_color(label_status, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        }
        xSemaphoreGive(xGuiSemaphore);
    }
}

void ui_set_ip_address(const char* ip)
{
    if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
        lv_label_set_text_fmt(label_ip, "IP: %s", ip);
        xSemaphoreGive(xGuiSemaphore);
    }
}

void ui_set_streaming_status(bool streaming)
{
    if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
        if (streaming) {
            lv_label_set_text(label_status, "[* REC] LIVE");
            lv_obj_set_style_text_color(label_status, lv_color_hex(0xFF0000), LV_PART_MAIN); // Red dot
        } else {
            lv_label_set_text(label_status, "Ready");
            lv_obj_set_style_text_color(label_status, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        }
        xSemaphoreGive(xGuiSemaphore);
    }
}

void ui_set_rssi(int rssi)
{
    if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
        lv_label_set_text_fmt(label_rssi, "WiFi: %d dBm", rssi);
        xSemaphoreGive(xGuiSemaphore);
    }
}

void ui_set_audio_level(int db)
{
    if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
        if (db < -60) db = -60;
        if (db > 0) db = 0;
        
        lv_bar_set_value(bar_vu, db, LV_ANIM_OFF);
        lv_label_set_text_fmt(label_db, "%d dB", db);

        // Color coding based on dB level
        if (db > -15) {
            lv_obj_set_style_bg_color(bar_vu, lv_color_hex(0xFF0000), LV_PART_INDICATOR); // Red
        } else if (db > -30) {
            lv_obj_set_style_bg_color(bar_vu, lv_color_hex(0xFFFF00), LV_PART_INDICATOR); // Yellow
        } else {
            lv_obj_set_style_bg_color(bar_vu, lv_color_hex(0x00FF00), LV_PART_INDICATOR); // Green
        }
        
        xSemaphoreGive(xGuiSemaphore);
    }
}
