#include "obd.h"
#include "dtc.h"
#include "profile.h"
#include "signals.h"
#include "twai_port.h"

#include "esp_timer.h"
#include <string.h>

static int s_pid_idx;
static int64_t s_last_us;
static uint8_t s_isotp_buf[64];
static int s_isotp_len;
static int s_isotp_expect;
static uint8_t s_isotp_seq;

static const uint8_t k_pids[] = { 0x01, 0x0C, 0x0D, 0x05, 0x0F, 0x11, 0x2F, 0x42 };

static void send_obd(const uint8_t *data, uint8_t n)
{
    can_frame_t m = { 0 };
    m.identifier = 0x7DF;
    m.data_length_code = 8;
    memset(m.data, 0x00, 8);
    m.data[0] = n;
    memcpy(m.data + 1, data, n);
    twai_port_send(&m);
}

void obd_init(void)
{
    s_pid_idx = 0;
}

static void apply_pid(uint8_t pid, const uint8_t *a, int n)
{
    switch (pid) {
    case 0x01:
        if (n >= 1) {
            bool mil = a[0] & 0x80;
            signals_set(SIG_MIL, mil ? 1.f : 0.f, SIG_SRC_OBD);
        }
        break;
    case 0x0C:
        if (n >= 2) {
            signals_set(SIG_RPM, ((a[0] * 256) + a[1]) / 4.f, SIG_SRC_OBD);
        }
        break;
    case 0x0D:
        if (n >= 1) {
            signals_set(SIG_SPEED_KPH, a[0], SIG_SRC_OBD);
        }
        break;
    case 0x05:
        if (n >= 1) {
            signals_set(SIG_COOLANT_C, a[0] - 40.f, SIG_SRC_OBD);
        }
        break;
    case 0x0F:
        break;
    case 0x11:
        break;
    case 0x2F:
        if (n >= 1) {
            signals_set(SIG_FUEL_PCT, a[0] * 100.f / 255.f, SIG_SRC_OBD);
        }
        break;
    case 0x42:
        if (n >= 2) {
            signals_set(SIG_VOLT, ((a[0] * 256) + a[1]) / 1000.f, SIG_SRC_OBD);
        }
        break;
    default:
        break;
    }
}

static void handle_payload(const uint8_t *p, int n)
{
    if (n < 2) {
        return;
    }
    if (p[0] == 0x41 && n >= 3) {
        apply_pid(p[1], p + 2, n - 2);
    } else if (p[0] == 0x43) {
        int ndtc = p[1];
        for (int i = 0; i < ndtc && (2 + i * 2 + 1) < n; i++) {
            uint16_t code = ((uint16_t)p[2 + i * 2] << 8) | p[3 + i * 2];
            if (code) {
                dtc_apply_obd(code, false, true);
            }
        }
    } else if (p[0] == 0x47) {
        int ndtc = p[1];
        for (int i = 0; i < ndtc && (2 + i * 2 + 1) < n; i++) {
            uint16_t code = ((uint16_t)p[2 + i * 2] << 8) | p[3 + i * 2];
            if (code) {
                dtc_apply_obd(code, true, false);
            }
        }
    }
}

void obd_on_frame(const can_frame_t *msg)
{
    if (msg->extd) {
        return;
    }
    uint32_t id = msg->identifier;
    if (id < 0x7E8 || id > 0x7EF) {
        return;
    }
    const uint8_t *d = msg->data;
    uint8_t pci = d[0] >> 4;
    if (pci == 0) {
        int n = d[0] & 0x0F;
        if (n > 7) {
            n = 7;
        }
        handle_payload(d + 1, n);
        s_isotp_expect = 0;
    } else if (pci == 1) {
        s_isotp_expect = ((d[0] & 0x0F) << 8) | d[1];
        int copy = (s_isotp_expect > 6) ? 6 : s_isotp_expect;
        memcpy(s_isotp_buf, d + 2, copy);
        s_isotp_len = copy;
        s_isotp_seq = 1;
        can_frame_t fc = { 0 };
        fc.identifier = 0x7E0 + (id - 0x7E8);
        fc.data_length_code = 8;
        fc.data[0] = 0x30;
        twai_port_send(&fc);
    } else if (pci == 2 && s_isotp_expect) {
        uint8_t seq = d[0] & 0x0F;
        if (seq == (s_isotp_seq & 0x0F)) {
            int room = (int)sizeof(s_isotp_buf) - s_isotp_len;
            int copy = 7;
            if (copy > room) {
                copy = room;
            }
            memcpy(s_isotp_buf + s_isotp_len, d + 1, copy);
            s_isotp_len += copy;
            s_isotp_seq++;
            if (s_isotp_len >= s_isotp_expect) {
                handle_payload(s_isotp_buf, s_isotp_expect);
                s_isotp_expect = 0;
            }
        }
    }
}

void obd_poll(void)
{
    dash_profile_t *p = profile_get();
    if (!profile_use_obd() || !p->obd_requests) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (now - s_last_us < 200000) {
        return;
    }
    s_last_us = now;
    if (s_pid_idx < (int)(sizeof(k_pids))) {
        uint8_t req[2] = { 0x01, k_pids[s_pid_idx++] };
        send_obd(req, 2);
    } else if (s_pid_idx == (int)sizeof(k_pids)) {
        uint8_t req[1] = { 0x03 };
        send_obd(req, 1);
        s_pid_idx++;
    } else {
        uint8_t req[1] = { 0x07 };
        send_obd(req, 1);
        s_pid_idx = 0;
    }
}
