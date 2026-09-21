#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PROFILE_J1939_250 = 0,
    PROFILE_J1939_500,
    PROFILE_OBD_500,
    PROFILE_MIXED_250,
    PROFILE_PROP_CUSTOM,
    PROFILE_COUNT
} vehicle_profile_t;

typedef struct {
    vehicle_profile_t profile;
    bool j1939_tx_enable;
    bool obd_requests;
    bool metric;
    uint8_t our_sa;
    uint8_t engine_sa;
    uint16_t tsc1_rpm_cap;
    uint8_t brightness;
    uint32_t prop_bitrate;
    bool prop_extended;
} dash_profile_t;

void profile_init(void);
dash_profile_t *profile_get(void);
void profile_save(void);
uint32_t profile_bitrate(void);
bool profile_use_j1939(void);
bool profile_use_obd(void);
bool profile_use_prop(void);
bool profile_listen_only(void);
const char *profile_name(vehicle_profile_t p);
