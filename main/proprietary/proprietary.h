#pragma once

#include "twai_port.h"
#include "signals.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t id;
    uint32_t id_mask;
    bool extended;
    uint8_t dlc;
    bool has_mux;
    uint16_t mux_start;
    uint8_t mux_len;
    uint32_t mux_val;
    signal_id_t sid;
    uint16_t start_bit;
    uint8_t length;
    bool intel;
    bool is_signed;
    float scale;
    float offset;
} proprietary_entry_t;

bool proprietary_on_frame(const can_frame_t *msg);
void proprietary_init_default(void);
esp_err_t proprietary_load_json(const char *json, size_t len);
int proprietary_message_count(void);
esp_err_t proprietary_add(const proprietary_entry_t *entry);
esp_err_t proprietary_save_nvs(void);
esp_err_t proprietary_export_json(char *buf, size_t cap, size_t *out_len);
