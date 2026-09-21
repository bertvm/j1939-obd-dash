#include "j1939_tx.h"
#include "j1939.h"
#include "twai_port.h"
#include "profile.h"
#include "signals.h"

#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "j1939_tx";
static bool s_claimed;
static bool s_tsc1_hold;
static uint16_t s_tsc1_rpm;
static int64_t s_tsc1_last_us;
static bool s_regen_force;
static bool s_regen_inhibit;
static int64_t s_regen_last_us;
static int s_cancel_left;
static const char *s_gate = "TX off";

#define TSC1_PERIOD_US 10000
#define REGEN_PERIOD_US 1000000
#define TSC1_HARD_CAP 4000

static bool interlock_ok(bool for_tsc1)
{
    float speed = 0, park = 0;
    uint32_t age;
    signal_quality_t q;
    bool have_speed = signals_get(SIG_SPEED_KPH, &speed, &q, &age) && q == SIG_Q_OK;
    bool have_park = signals_get(SIG_PARK, &park, &q, &age) && q == SIG_Q_OK;
    if (have_speed && speed > 1.5f) {
        s_gate = "Speed too high";
        return false;
    }
    if (for_tsc1 && have_park && park < 0.5f) {
        s_gate = "Park brake";
        return false;
    }
    return true;
}

const char *j1939_tx_gate_reason(void)
{
    dash_profile_t *p = profile_get();
    if (!profile_use_j1939()) {
        return "Not a J1939 profile";
    }
    if (!p->j1939_tx_enable || profile_listen_only()) {
        return "Enable J1939 transmit";
    }
    if (!s_claimed) {
        return "Address not claimed";
    }
    if (twai_port_bus_off()) {
        return "Bus-off";
    }
    if (twai_port_err_passive()) {
        return "Error-passive";
    }
    return s_gate;
}

static bool gate_ok(bool for_tsc1)
{
    dash_profile_t *p = profile_get();
    if (!profile_use_j1939() || !p->j1939_tx_enable || profile_listen_only()) {
        return false;
    }
    if (!s_claimed) {
        return false;
    }
    if (twai_port_bus_off() || twai_port_err_passive()) {
        s_tsc1_hold = false;
        s_regen_force = false;
        return false;
    }
    if (!interlock_ok(for_tsc1)) {
        return false;
    }
    s_gate = "TX armed";
    return true;
}

static void send_pgn(uint8_t prio, uint32_t pgn, uint8_t da, const uint8_t data[8])
{
    dash_profile_t *p = profile_get();
    can_frame_t m = { 0 };
    m.identifier = j1939_make_id(prio, pgn, da, p->our_sa);
    m.extd = 1;
    m.data_length_code = 8;
    memcpy(m.data, data, 8);
    twai_port_send(&m);
}

esp_err_t j1939_tx_claim(void)
{
    dash_profile_t *p = profile_get();
    if (!p->j1939_tx_enable) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t name[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x80 };
    send_pgn(6, 60928, 0xFF, name);
    s_claimed = true;
    ESP_LOGI(TAG, "address claim SA=0x%02X", p->our_sa);
    return ESP_OK;
}

void j1939_tx_init(void)
{
    s_claimed = false;
}

bool j1939_tx_claimed(void)
{
    return s_claimed;
}

void j1939_tsc1_hold(bool hold, uint16_t rpm)
{
    dash_profile_t *p = profile_get();
    if (rpm > p->tsc1_rpm_cap) {
        rpm = p->tsc1_rpm_cap;
    }
    if (rpm > TSC1_HARD_CAP) {
        rpm = TSC1_HARD_CAP;
    }
    s_tsc1_rpm = rpm;
    s_tsc1_hold = hold;
    if (!hold) {
        s_tsc1_last_us = 0;
    }
}

void j1939_regen_force(bool on)
{
    s_regen_force = on;
    if (on) {
        s_regen_inhibit = false;
    }
}

void j1939_regen_inhibit(bool on)
{
    s_regen_inhibit = on;
    if (on) {
        s_regen_force = false;
    }
}

void j1939_regen_cancel(void)
{
    s_regen_force = false;
    s_regen_inhibit = false;
    s_cancel_left = 3;
}

bool j1939_tsc1_active(void)
{
    return s_tsc1_hold;
}

bool j1939_regen_active(void)
{
    return s_regen_force || s_regen_inhibit;
}

static void send_tsc1(void)
{
    dash_profile_t *p = profile_get();
    uint16_t raw = (uint16_t)(s_tsc1_rpm / 0.125f);
    uint8_t d[8];
    memset(d, 0xFF, 8);
    d[0] = 0x01; /* speed control, unused bits NA later */
    d[1] = (uint8_t)(raw & 0xFF);
    d[2] = (uint8_t)(raw >> 8);
    send_pgn(3, 0, p->engine_sa, d);
}

static void send_dpfc1(bool force, bool inhibit)
{
    uint8_t d[8];
    memset(d, 0xFF, 8);
    uint8_t inh = inhibit ? 1 : 0;
    uint8_t frc = force ? 1 : 0;
    d[0] = (uint8_t)((inh & 3) | ((frc & 3) << 2) | 0xF0);
    send_pgn(6, 58112, 0xFF, d);
}

void j1939_tx_poll(void)
{
    int64_t now = esp_timer_get_time();
    if (s_tsc1_hold && gate_ok(true)) {
        if (now - s_tsc1_last_us >= TSC1_PERIOD_US) {
            send_tsc1();
            s_tsc1_last_us = now;
        }
    } else if (s_tsc1_hold && !gate_ok(true)) {
        s_tsc1_hold = false;
    }

    if (s_cancel_left > 0 && gate_ok(false)) {
        if (now - s_regen_last_us >= 200000) {
            send_dpfc1(false, false);
            s_regen_last_us = now;
            s_cancel_left--;
        }
        return;
    }
    if ((s_regen_force || s_regen_inhibit) && gate_ok(false)) {
        if (now - s_regen_last_us >= REGEN_PERIOD_US) {
            send_dpfc1(s_regen_force, s_regen_inhibit);
            s_regen_last_us = now;
        }
    } else if ((s_regen_force || s_regen_inhibit) && !gate_ok(false)) {
        s_regen_force = false;
        s_regen_inhibit = false;
    }
}
