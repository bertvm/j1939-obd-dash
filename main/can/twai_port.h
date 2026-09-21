#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t identifier;
    uint8_t data[8];
    uint8_t data_length_code;
    bool extd;
    bool rtr;
} can_frame_t;

typedef void (*twai_rx_cb_t)(const can_frame_t *msg, void *ctx);

esp_err_t twai_port_start(void);
esp_err_t twai_port_restart(void);
esp_err_t twai_port_send(const can_frame_t *msg);
bool twai_port_bus_off(void);
bool twai_port_err_passive(void);
void twai_port_set_rx_cb(twai_rx_cb_t cb, void *ctx);
void twai_unmatched_note(uint32_t id, bool ext, const uint8_t *data, uint8_t dlc);
int twai_unmatched_count(void);
void twai_unmatched_get(int idx, uint32_t *id, uint32_t *count, uint8_t *dlc, uint8_t data[8]);
void twai_live_set_enabled(bool on);
void twai_live_clear(void);
int twai_live_count(void);
void twai_live_get(int idx, uint32_t *id, uint32_t *count, uint8_t *dlc, uint8_t data[8], bool *extd);
