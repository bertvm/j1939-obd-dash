#include "twai_port.h"

#include "board.h"
#include "pins.h"
#include "profile.h"
#include "signals.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "twai";
static twai_rx_cb_t s_cb;
static void *s_ctx;
static volatile bool s_bus_off;
static volatile bool s_err_pass;
static bool s_started;
static twai_node_handle_t s_node;
static QueueHandle_t s_rxq;

typedef struct {
    uint32_t id;
    uint32_t count;
    uint8_t dlc;
    uint8_t data[8];
} unmatched_t;

#define UNMATCHED_MAX 32
static unmatched_t s_un[UNMATCHED_MAX];
static int s_un_n;
static portMUX_TYPE s_un_mux = portMUX_INITIALIZER_UNLOCKED;

void twai_unmatched_note(uint32_t id, bool ext, const uint8_t *data, uint8_t dlc)
{
    (void)ext;
    portENTER_CRITICAL(&s_un_mux);
    for (int i = 0; i < s_un_n; i++) {
        if (s_un[i].id == id) {
            s_un[i].count++;
            s_un[i].dlc = dlc;
            for (int b = 0; b < dlc && b < 8; b++) {
                s_un[i].data[b] = data[b];
            }
            portEXIT_CRITICAL(&s_un_mux);
            return;
        }
    }
    if (s_un_n < UNMATCHED_MAX) {
        s_un[s_un_n].id = id;
        s_un[s_un_n].count = 1;
        s_un[s_un_n].dlc = dlc;
        for (int b = 0; b < dlc && b < 8; b++) {
            s_un[s_un_n].data[b] = data[b];
        }
        s_un_n++;
    }
    portEXIT_CRITICAL(&s_un_mux);
}

#define LIVE_MAX 24
typedef struct {
    uint32_t id;
    uint32_t count;
    int64_t last_us;
    uint8_t dlc;
    uint8_t data[8];
    bool extd;
} live_slot_t;

static live_slot_t s_live[LIVE_MAX];
static int s_live_n;
static bool s_live_on;
static portMUX_TYPE s_live_mux = portMUX_INITIALIZER_UNLOCKED;

void twai_live_set_enabled(bool on)
{
    s_live_on = on;
}

void twai_live_clear(void)
{
    portENTER_CRITICAL(&s_live_mux);
    s_live_n = 0;
    memset(s_live, 0, sizeof(s_live));
    portEXIT_CRITICAL(&s_live_mux);
}

static void live_note(const can_frame_t *msg)
{
    if (!s_live_on) {
        return;
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_live_mux);
    for (int i = 0; i < s_live_n; i++) {
        if (s_live[i].id == msg->identifier && s_live[i].extd == msg->extd) {
            s_live[i].count++;
            s_live[i].last_us = now;
            s_live[i].dlc = msg->data_length_code;
            memcpy(s_live[i].data, msg->data, 8);
            portEXIT_CRITICAL(&s_live_mux);
            return;
        }
    }
    int slot = s_live_n;
    if (slot >= LIVE_MAX) {
        slot = 0;
        for (int i = 1; i < LIVE_MAX; i++) {
            if (s_live[i].last_us < s_live[slot].last_us) {
                slot = i;
            }
        }
    } else {
        s_live_n++;
    }
    s_live[slot].id = msg->identifier;
    s_live[slot].extd = msg->extd;
    s_live[slot].count = 1;
    s_live[slot].last_us = now;
    s_live[slot].dlc = msg->data_length_code;
    memcpy(s_live[slot].data, msg->data, 8);
    portEXIT_CRITICAL(&s_live_mux);
}

int twai_live_count(void)
{
    return s_live_n;
}

void twai_live_get(int idx, uint32_t *id, uint32_t *count, uint8_t *dlc, uint8_t data[8], bool *extd)
{
    portENTER_CRITICAL(&s_live_mux);
    if (idx >= 0 && idx < s_live_n) {
        if (id) {
            *id = s_live[idx].id;
        }
        if (count) {
            *count = s_live[idx].count;
        }
        if (dlc) {
            *dlc = s_live[idx].dlc;
        }
        if (extd) {
            *extd = s_live[idx].extd;
        }
        if (data) {
            memcpy(data, s_live[idx].data, 8);
        }
    }
    portEXIT_CRITICAL(&s_live_mux);
}

int twai_unmatched_count(void)
{
    return s_un_n;
}

void twai_unmatched_get(int idx, uint32_t *id, uint32_t *count, uint8_t *dlc, uint8_t data[8])
{
    portENTER_CRITICAL(&s_un_mux);
    if (idx >= 0 && idx < s_un_n) {
        if (id) {
            *id = s_un[idx].id;
        }
        if (count) {
            *count = s_un[idx].count;
        }
        if (dlc) {
            *dlc = s_un[idx].dlc;
        }
        if (data) {
            for (int b = 0; b < 8; b++) {
                data[b] = s_un[idx].data[b];
            }
        }
    }
    portEXIT_CRITICAL(&s_un_mux);
}

static bool IRAM_ATTR on_rx_done(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *user_ctx)
{
    (void)edata;
    (void)user_ctx;
    uint8_t buf[8];
    twai_frame_t frame = {
        .buffer = buf,
        .buffer_len = sizeof(buf),
    };
    if (twai_node_receive_from_isr(handle, &frame) != ESP_OK) {
        return false;
    }
    can_frame_t msg = { 0 };
    msg.identifier = frame.header.id;
    msg.extd = frame.header.ide;
    msg.rtr = frame.header.rtr;
    msg.data_length_code = frame.header.dlc > 8 ? 8 : (uint8_t)frame.header.dlc;
    memcpy(msg.data, buf, msg.data_length_code);
    BaseType_t woke = pdFALSE;
    xQueueSendFromISR(s_rxq, &msg, &woke);
    return woke == pdTRUE;
}

static bool IRAM_ATTR on_state(twai_node_handle_t handle, const twai_state_change_event_data_t *edata, void *user_ctx)
{
    (void)handle;
    (void)user_ctx;
    if (edata->new_sta == TWAI_ERROR_BUS_OFF) {
        s_bus_off = true;
    }
    if (edata->new_sta == TWAI_ERROR_PASSIVE) {
        s_err_pass = true;
    }
    return false;
}

static void rx_task(void *arg)
{
    (void)arg;
    can_frame_t msg;
    while (1) {
        if (xQueueReceive(s_rxq, &msg, pdMS_TO_TICKS(50)) == pdTRUE) {
            signals_touch_bus();
            live_note(&msg);
            if (s_cb) {
                s_cb(&msg, s_ctx);
            }
        }
    }
}

static esp_err_t install_driver(void)
{
    board_set_can_mode(true);
    vTaskDelay(pdMS_TO_TICKS(20));
    if (!s_rxq) {
        s_rxq = xQueueCreate(32, sizeof(can_frame_t));
    }

    twai_onchip_node_config_t cfg = {
        .io_cfg = {
            .tx = BOARD_TWAI_TX,
            .rx = BOARD_TWAI_RX,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing.bitrate = profile_bitrate(),
        .fail_retry_cnt = profile_listen_only() ? 0 : 3,
        .tx_queue_depth = 16,
        .flags.enable_listen_only = profile_listen_only(),
    };
    esp_err_t err = twai_new_node_onchip(&cfg, &s_node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "node create failed %s", esp_err_to_name(err));
        return err;
    }
    twai_event_callbacks_t cbs = {
        .on_rx_done = on_rx_done,
        .on_state_change = on_state,
    };
    ESP_RETURN_ON_ERROR(twai_node_register_event_callbacks(s_node, &cbs, NULL), TAG, "cbs");
    ESP_RETURN_ON_ERROR(twai_node_enable(s_node), TAG, "enable");
    s_bus_off = false;
    s_err_pass = false;
    s_started = true;
    ESP_LOGI(TAG, "TWAI %lu bps listen=%d TX=%d RX=%d",
             (unsigned long)profile_bitrate(), profile_listen_only(), BOARD_TWAI_TX, BOARD_TWAI_RX);
    return ESP_OK;
}

esp_err_t twai_port_start(void)
{
    esp_err_t err = install_driver();
    if (err != ESP_OK) {
        return err;
    }
    xTaskCreatePinnedToCore(rx_task, "twai_rx", 4096, NULL, 20, NULL, 0);
    return ESP_OK;
}

esp_err_t twai_port_restart(void)
{
    if (s_started && s_node) {
        twai_node_disable(s_node);
        twai_node_delete(s_node);
        s_node = NULL;
        s_started = false;
    }
    return install_driver();
}

esp_err_t twai_port_send(const can_frame_t *msg)
{
    if (profile_listen_only() || !s_node) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[8];
    memcpy(buf, msg->data, 8);
    twai_frame_t frame = {
        .header.id = msg->identifier,
        .header.dlc = msg->data_length_code,
        .header.ide = msg->extd,
        .header.rtr = msg->rtr,
        .buffer = buf,
        .buffer_len = msg->data_length_code,
    };
    return twai_node_transmit(s_node, &frame, 20);
}

bool twai_port_bus_off(void)
{
    return s_bus_off;
}

bool twai_port_err_passive(void)
{
    return s_err_pass;
}

void twai_port_set_rx_cb(twai_rx_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_ctx = ctx;
}
