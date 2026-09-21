#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DTC_MAX 24

typedef enum {
    DTC_KIND_J1939 = 0,
    DTC_KIND_OBD,
} dtc_kind_t;

typedef struct {
    dtc_kind_t kind;
    uint32_t spn;
    uint8_t fmi;
    uint8_t oc;
    uint8_t sa;
    uint8_t lamps;
    uint16_t obd_code;
    char title[96];
    char sub[96];
    bool pending;
    int64_t last_us;
} dtc_entry_t;

void dtc_init(void);
void dtc_apply_dm1(uint8_t sa, const uint8_t *payload, uint16_t len);
void dtc_apply_obd(uint16_t code, bool pending, bool mil);
int dtc_count(void);
const dtc_entry_t *dtc_get(int idx);
void dtc_format_obd(uint16_t code, char *out, int out_len);
const char *dtc_fmi_text(uint8_t fmi);
const char *dtc_spn_name(uint32_t spn);
void dtc_add_spn_name(uint32_t spn, const char *name);
void dtc_add_obd_title(uint16_t code, const char *title);
bool dtc_bus_snapshot_stale(void);
