#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SIG_RPM = 0,
    SIG_SPEED_KPH,
    SIG_COOLANT_C,
    SIG_OIL_KPA,
    SIG_OIL_C,
    SIG_VOLT,
    SIG_FUEL_PCT,
    SIG_DEF_PCT,
    SIG_FUEL_RATE,
    SIG_HOURS,
    SIG_TORQUE_PCT,
    SIG_PARK,
    SIG_MIL,
    SIG_RED_STOP,
    SIG_AMBER,
    SIG_PROTECT,
    SIG_DPF,
    SIG_HEST,
    SIG_WAIT_START,
    SIG_WIF,
    SIG_DPF_REGEN,
    SIG_COUNT
} signal_id_t;

typedef enum {
    SIG_SRC_NONE = 0,
    SIG_SRC_J1939,
    SIG_SRC_OBD,
    SIG_SRC_PROP,
} signal_src_t;

typedef enum {
    SIG_Q_STALE = 0,
    SIG_Q_OK,
    SIG_Q_WARN,
    SIG_Q_ALARM,
} signal_quality_t;

void signals_init(void);
void signals_set(signal_id_t id, float value, signal_src_t src);
void signals_set_override(signal_id_t id, float value, signal_src_t src);
bool signals_get(signal_id_t id, float *value, signal_quality_t *q, uint32_t *age_ms);
void signals_touch_bus(void);
bool signals_bus_lost(void);
const char *signal_name(signal_id_t id);
const char *signal_unit(signal_id_t id);
void signals_set_stale_ms(uint32_t ms);
