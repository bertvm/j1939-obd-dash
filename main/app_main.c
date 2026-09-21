#include "board.h"
#include "dtc.h"
#include "j1939.h"
#include "j1939_tx.h"
#include "obd.h"
#include "profile.h"
#include "proprietary.h"
#include "sd_fs.h"
#include "signals.h"
#include "twai_port.h"
#include "ui.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "app";

static void on_can(const can_frame_t *msg, void *ctx)
{
    (void)ctx;
    bool hit = false;
    if (profile_use_prop()) {
        hit = proprietary_on_frame(msg);
    }
    if (profile_use_j1939()) {
        j1939_on_frame(msg);
    }
    if (profile_use_obd()) {
        obd_on_frame(msg);
    }
    if (!hit && profile_use_prop() == false) {
        /* unmatched only tracked in proprietary profiles; still useful on mixed */
    }
}

static void tick_timer(void *arg)
{
    (void)arg;
    lv_tick_inc(2);
}

static void work_task(void *arg)
{
    (void)arg;
    int n = 0;
    while (1) {
        j1939_tx_poll();
        obd_poll();
        if ((n++ % 50) == 0) {
            sd_fs_log_signals();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "J1939/OBD dash Waveshare ESP32-S3-Touch-LCD-7 rev 1.2");
    profile_init();
    signals_init();
    dtc_init();
    j1939_init();
    j1939_tx_init();
    obd_init();
    proprietary_init_default();

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(board_init(&panel, &touch));
    ESP_ERROR_CHECK(ui_start(panel, touch));

    const esp_timer_create_args_t tick_args = {
        .callback = tick_timer,
        .name = "lv_tick",
    };
    esp_timer_handle_t tick;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick, 2000));

    twai_port_set_rx_cb(on_can, NULL);
    ESP_ERROR_CHECK(twai_port_start());

    if (sd_fs_init() != ESP_OK) {
        ESP_LOGW(TAG, "SD optional — continuing without card");
    }

    xTaskCreatePinnedToCore(work_task, "work", 4096, NULL, 8, NULL, 0);
    ESP_LOGI(TAG, "running  bitrate=%lu listen=%d", (unsigned long)profile_bitrate(), profile_listen_only());
}
