#include "signals.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    float value;
    signal_src_t src;
    int64_t ts_us;
    bool valid;
} slot_t;

static slot_t s_slots[SIG_COUNT];
static SemaphoreHandle_t s_mu;
static int64_t s_bus_us;
static uint32_t s_stale_ms = 3000;

static const char *const k_names[SIG_COUNT] = {
    "rpm", "speed", "coolant", "oil_p", "oil_t", "volt", "fuel", "def",
    "fuel_rate", "hours", "torque", "park", "mil", "red_stop", "amber",
    "protect", "dpf", "hest", "wait_start", "wif", "dpf_regen",
};

static const char *const k_units[SIG_COUNT] = {
    "rpm", "km/h", "C", "kPa", "C", "V", "%", "%",
    "L/h", "h", "%", "", "", "", "",
    "", "", "", "", "", "",
};

void signals_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    s_bus_us = 0;
}

void signals_set_stale_ms(uint32_t ms)
{
    s_stale_ms = ms;
}

static void set_inner(signal_id_t id, float value, signal_src_t src, bool override)
{
    if (id >= SIG_COUNT) {
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (!override && s_slots[id].src == SIG_SRC_PROP && src != SIG_SRC_PROP && s_slots[id].valid) {
        int64_t age = esp_timer_get_time() - s_slots[id].ts_us;
        if (age < (int64_t)s_stale_ms * 1000) {
            xSemaphoreGive(s_mu);
            return;
        }
    }
    s_slots[id].value = value;
    s_slots[id].src = src;
    s_slots[id].ts_us = esp_timer_get_time();
    s_slots[id].valid = true;
    s_bus_us = s_slots[id].ts_us;
    xSemaphoreGive(s_mu);
}

void signals_set(signal_id_t id, float value, signal_src_t src)
{
    set_inner(id, value, src, false);
}

void signals_set_override(signal_id_t id, float value, signal_src_t src)
{
    set_inner(id, value, src, true);
}

void signals_touch_bus(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_bus_us = esp_timer_get_time();
    xSemaphoreGive(s_mu);
}

bool signals_get(signal_id_t id, float *value, signal_quality_t *q, uint32_t *age_ms)
{
    if (id >= SIG_COUNT) {
        return false;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    slot_t s = s_slots[id];
    xSemaphoreGive(s_mu);
    if (!s.valid) {
        if (q) {
            *q = SIG_Q_STALE;
        }
        return false;
    }
    uint32_t age = (uint32_t)((esp_timer_get_time() - s.ts_us) / 1000);
    if (age_ms) {
        *age_ms = age;
    }
    if (value) {
        *value = s.value;
    }
    if (q) {
        *q = (age > s_stale_ms) ? SIG_Q_STALE : SIG_Q_OK;
    }
    return true;
}

bool signals_bus_lost(void)
{
    if (s_bus_us == 0) {
        return true;
    }
    return (esp_timer_get_time() - s_bus_us) > (int64_t)s_stale_ms * 2000;
}

const char *signal_name(signal_id_t id)
{
    return (id < SIG_COUNT) ? k_names[id] : "?";
}

const char *signal_unit(signal_id_t id)
{
    return (id < SIG_COUNT) ? k_units[id] : "";
}
