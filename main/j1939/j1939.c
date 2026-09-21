#include "j1939.h"
#include "dtc.h"
#include "signals.h"
#include "profile.h"

#include <string.h>

#define PGN_EEC1     61444u
#define PGN_CCVS     65265u
#define PGN_ET1      65262u
#define PGN_EFLP1    65263u
#define PGN_VEP1     65271u
#define PGN_LFE      65266u
#define PGN_DD       65276u
#define PGN_HOURS    65253u
#define PGN_DM1      65226u
#define PGN_AT1T1I   65110u
#define PGN_DPFC1    58112u
#define PGN_TP_CM    60416u
#define PGN_TP_DT    60160u

uint32_t j1939_pgn_from_id(uint32_t id)
{
    uint8_t pf = (id >> 16) & 0xFF;
    uint8_t ps = (id >> 8) & 0xFF;
    if (pf < 240) {
        return ((uint32_t)pf) << 8;
    }
    return ((uint32_t)pf << 8) | ps;
}

uint8_t j1939_sa_from_id(uint32_t id)
{
    return (uint8_t)(id & 0xFF);
}

uint8_t j1939_da_from_id(uint32_t id)
{
    uint8_t pf = (id >> 16) & 0xFF;
    if (pf < 240) {
        return (uint8_t)((id >> 8) & 0xFF);
    }
    return 0xFF;
}

uint32_t j1939_make_id(uint8_t prio, uint32_t pgn, uint8_t da, uint8_t sa)
{
    uint8_t pf = (uint8_t)((pgn >> 8) & 0xFF);
    uint32_t id = ((uint32_t)(prio & 7) << 26) | sa;
    if (pf < 240) {
        id |= ((uint32_t)pf << 16) | ((uint32_t)da << 8);
    } else {
        id |= (pgn & 0x1FFFF) << 8;
    }
    return id;
}

static uint16_t u16le(const uint8_t *d, int i)
{
    return (uint16_t)d[i] | ((uint16_t)d[i + 1] << 8);
}

static uint32_t u32le(const uint8_t *d, int i)
{
    return (uint32_t)d[i] | ((uint32_t)d[i + 1] << 8) | ((uint32_t)d[i + 2] << 16) | ((uint32_t)d[i + 3] << 24);
}

static bool nav_u8(uint8_t v)
{
    return v < 0xFB;
}

/* BAM reassembly for DM1 */
static uint8_t s_bam[1785];
static uint16_t s_bam_size;
static uint8_t s_bam_next;
static uint8_t s_bam_sa;
static bool s_bam_on;

static void parse_dm1_payload(uint8_t sa, const uint8_t *p, uint16_t n)
{
    if (n < 2) {
        return;
    }
    dtc_apply_dm1(sa, p, n);
    uint8_t lamps = p[0];
    signals_set(SIG_MIL, (lamps & 0xC0) ? 1.f : 0.f, SIG_SRC_J1939);
    signals_set(SIG_RED_STOP, (lamps & 0x30) ? 1.f : 0.f, SIG_SRC_J1939);
    signals_set(SIG_AMBER, (lamps & 0x0C) ? 1.f : 0.f, SIG_SRC_J1939);
    signals_set(SIG_PROTECT, (lamps & 0x03) ? 1.f : 0.f, SIG_SRC_J1939);
}

static void decode_pgn(uint32_t pgn, uint8_t sa, const uint8_t *d, uint8_t len)
{
    (void)sa;
    if (len < 1) {
        return;
    }
    switch (pgn) {
    case PGN_EEC1:
        if (len >= 5) {
            uint16_t raw = u16le(d, 3);
            if (raw < 0xFAFF) {
                signals_set(SIG_RPM, raw * 0.125f, SIG_SRC_J1939);
            }
            if (nav_u8(d[2])) {
                signals_set(SIG_TORQUE_PCT, (float)d[2] - 125.f, SIG_SRC_J1939);
            }
        }
        break;
    case PGN_CCVS:
        if (len >= 4) {
            uint16_t raw = u16le(d, 1);
            if (raw < 0xFAFF) {
                signals_set(SIG_SPEED_KPH, raw / 256.f, SIG_SRC_J1939);
            }
            uint8_t park = (d[3] >> 2) & 0x03;
            if (park < 3) {
                signals_set(SIG_PARK, park == 1 ? 1.f : 0.f, SIG_SRC_J1939);
            }
        }
        break;
    case PGN_ET1:
        if (nav_u8(d[0])) {
            signals_set(SIG_COOLANT_C, (float)d[0] - 40.f, SIG_SRC_J1939);
        }
        break;
    case PGN_EFLP1:
        if (len >= 4 && nav_u8(d[3])) {
            signals_set(SIG_OIL_KPA, d[3] * 4.f, SIG_SRC_J1939);
        }
        if (len >= 6) {
            uint16_t ot = u16le(d, 4);
            if (ot < 0xFAFF) {
                signals_set(SIG_OIL_C, ot * 0.03125f - 273.f, SIG_SRC_J1939);
            }
        }
        break;
    case PGN_VEP1:
        if (len >= 8) {
            uint16_t v = u16le(d, 6);
            if (v < 0xFAFF) {
                signals_set(SIG_VOLT, v * 0.05f, SIG_SRC_J1939);
            }
        }
        break;
    case PGN_LFE:
        if (len >= 2) {
            uint16_t fr = u16le(d, 0);
            if (fr < 0xFAFF) {
                signals_set(SIG_FUEL_RATE, fr * 0.05f, SIG_SRC_J1939);
            }
        }
        break;
    case PGN_DD:
        if (len >= 2 && nav_u8(d[1])) {
            signals_set(SIG_FUEL_PCT, d[1] * 0.4f, SIG_SRC_J1939);
        }
        break;
    case PGN_HOURS:
        if (len >= 4) {
            uint32_t h = u32le(d, 0);
            if (h < 0xFAFFFFFFu) {
                signals_set(SIG_HOURS, h * 0.05f, SIG_SRC_J1939);
            }
        }
        break;
    case PGN_AT1T1I:
        if (nav_u8(d[0])) {
            signals_set(SIG_DEF_PCT, d[0] * 0.4f, SIG_SRC_J1939);
        }
        break;
    case PGN_DPFC1:
        if (len >= 1) {
            uint8_t force = (d[0] >> 2) & 0x03;
            uint8_t hest = (len > 1) ? ((d[1] >> 2) & 0x03) : 0;
            if (force < 3) {
                signals_set(SIG_DPF_REGEN, force == 1 ? 1.f : 0.f, SIG_SRC_J1939);
                signals_set(SIG_DPF, force == 1 ? 1.f : 0.f, SIG_SRC_J1939);
            }
            if (hest == 1) {
                signals_set(SIG_HEST, 1.f, SIG_SRC_J1939);
            }
        }
        break;
    case PGN_DM1:
        parse_dm1_payload(sa, d, len);
        break;
    default:
        break;
    }
}

void j1939_init(void)
{
}

void j1939_on_frame(const can_frame_t *msg)
{
    if (!msg->extd || msg->rtr) {
        return;
    }
    uint32_t pgn = j1939_pgn_from_id(msg->identifier);
    uint8_t sa = j1939_sa_from_id(msg->identifier);

    if (pgn == PGN_TP_CM && msg->data_length_code >= 8 && msg->data[0] == 32) {
        uint16_t size = (uint16_t)msg->data[1] | ((uint16_t)msg->data[2] << 8);
        uint32_t tp_pgn = msg->data[5] | ((uint32_t)msg->data[6] << 8) | ((uint32_t)msg->data[7] << 16);
        if (tp_pgn == PGN_DM1 && size <= sizeof(s_bam)) {
            s_bam_on = true;
            s_bam_size = size;
            s_bam_next = 1;
            s_bam_sa = sa;
            memset(s_bam, 0xFF, sizeof(s_bam));
        }
        return;
    }
    if (pgn == PGN_TP_DT && s_bam_on && sa == s_bam_sa && msg->data_length_code >= 2) {
        uint8_t seq = msg->data[0];
        if (seq == s_bam_next) {
            uint16_t off = (uint16_t)(seq - 1) * 7;
            for (int i = 0; i < 7 && (off + i) < s_bam_size; i++) {
                s_bam[off + i] = msg->data[1 + i];
            }
            s_bam_next++;
            if (off + 7 >= s_bam_size) {
                parse_dm1_payload(sa, s_bam, s_bam_size);
                s_bam_on = false;
            }
        }
        return;
    }

    decode_pgn(pgn, sa, msg->data, msg->data_length_code);
}
