#include "dtc.h"
#include "signals.h"

#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static dtc_entry_t s_list[DTC_MAX];
static int s_n;

static const char *const k_fmi[32] = {
    "Data valid but above normal",
    "Data valid but below normal",
    "Data erratic or incorrect",
    "Voltage above normal",
    "Voltage below normal",
    "Current above normal",
    "Current below normal",
    "Mechanical system not responding",
    "Abnormal frequency or pulse width",
    "Abnormal update rate",
    "Abnormal rate of change",
    "Root cause not known",
    "Bad intelligent device",
    "Out of calibration",
    "Special instruction",
    "Data valid but above normal (least severe)",
    "Data valid but above normal (moderately severe)",
    "Data valid but below normal (least severe)",
    "Data valid but below normal (moderately severe)",
    "Received network data in error",
    "Data drifted high",
    "Data drifted low",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Condition exists",
};

typedef struct {
    uint32_t spn;
    const char *name;
} spn_name_t;

static const spn_name_t k_spn[] = {
    { 91, "Accelerator pedal position" },
    { 94, "Fuel delivery pressure" },
    { 97, "Water in fuel" },
    { 100, "Engine oil pressure" },
    { 102, "Intake manifold pressure" },
    { 105, "Intake manifold temperature" },
    { 110, "Engine coolant temperature" },
    { 111, "Coolant level" },
    { 175, "Engine oil temperature" },
    { 190, "Engine speed" },
    { 247, "Engine total hours" },
    { 512, "Driver demand torque" },
    { 513, "Actual engine torque" },
    { 1081, "Wait to start lamp" },
    { 1761, "DEF tank level" },
    { 3031, "DEF tank temperature" },
    { 3216, "Aftertreatment NOx" },
    { 3226, "Aftertreatment outlet NOx" },
    { 3251, "DPF differential pressure" },
    { 3697, "DPF lamp command" },
    { 3698, "Exhaust system high temperature lamp" },
    { 3700, "DPF regenerative status" },
    { 5246, "Aftertreatment SCR inducement" },
};

typedef struct {
    uint16_t code;
    const char *title;
} obd_title_t;

static const obd_title_t k_obd[] = {
    { 0x0000, "No DTCs" },
    { 0x0113, "IAT circuit high" },
    { 0x0118, "Coolant temp circuit high" },
    { 0x0128, "Coolant thermostat" },
    { 0x0171, "System too lean bank 1" },
    { 0x0217, "Engine coolant over-temperature" },
    { 0x0300, "Random/multiple misfire" },
    { 0x0420, "Catalyst efficiency bank 1" },
    { 0x0A1F, "Battery energy control module" },
};

#define EXTRA_MAX 64
static spn_name_t s_extra_spn[EXTRA_MAX];
static int s_extra_spn_n;
static obd_title_t s_extra_obd[EXTRA_MAX];
static int s_extra_obd_n;

const char *dtc_fmi_text(uint8_t fmi)
{
    return k_fmi[fmi & 31];
}

const char *dtc_spn_name(uint32_t spn)
{
    for (int i = 0; i < s_extra_spn_n; i++) {
        if (s_extra_spn[i].spn == spn) {
            return s_extra_spn[i].name;
        }
    }
    for (size_t i = 0; i < sizeof(k_spn) / sizeof(k_spn[0]); i++) {
        if (k_spn[i].spn == spn) {
            return k_spn[i].name;
        }
    }
    return NULL;
}

void dtc_add_spn_name(uint32_t spn, const char *name)
{
    if (!name || s_extra_spn_n >= EXTRA_MAX) {
        return;
    }
    s_extra_spn[s_extra_spn_n].spn = spn;
    s_extra_spn[s_extra_spn_n].name = strdup(name);
    s_extra_spn_n++;
}

void dtc_add_obd_title(uint16_t code, const char *title)
{
    if (!title || s_extra_obd_n >= EXTRA_MAX) {
        return;
    }
    s_extra_obd[s_extra_obd_n].code = code;
    s_extra_obd[s_extra_obd_n].title = strdup(title);
    s_extra_obd_n++;
}

void dtc_format_obd(uint16_t code, char *out, int out_len)
{
    const char letters[] = { 'P', 'C', 'B', 'U' };
    char letter = letters[(code >> 14) & 3];
    unsigned d1 = (code >> 12) & 3;
    unsigned rest = code & 0x0FFF;
    const char *title = NULL;
    for (int i = 0; i < s_extra_obd_n; i++) {
        if (s_extra_obd[i].code == code) {
            title = s_extra_obd[i].title;
            break;
        }
    }
    if (!title) {
        for (size_t i = 0; i < sizeof(k_obd) / sizeof(k_obd[0]); i++) {
            if (k_obd[i].code == code) {
                title = k_obd[i].title;
                break;
            }
        }
    }
    if (title) {
        snprintf(out, out_len, "%c%u%03X %s", letter, d1, rest, title);
    } else {
        snprintf(out, out_len, "%c%u%03X (no description on device)", letter, d1, rest);
    }
}

static const char *lamp_name(uint8_t lamps)
{
    if (lamps & 0x30) {
        return "Red stop";
    }
    if (lamps & 0xC0) {
        return "MIL";
    }
    if (lamps & 0x0C) {
        return "Amber warning";
    }
    if (lamps & 0x03) {
        return "Protect";
    }
    return "No lamp";
}

static int lamp_rank(uint8_t lamps)
{
    if (lamps & 0x30) {
        return 0;
    }
    if (lamps & 0xC0) {
        return 1;
    }
    if (lamps & 0x0C) {
        return 2;
    }
    return 3;
}

static uint32_t decode_spn(const uint8_t *d)
{
    /* Conversion method 4: SPN in 19 bits little-endian across first 2.5 bytes */
    uint32_t spn = d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)(d[2] & 0xE0) << 11);
    return spn;
}

static void sort_list(void)
{
    for (int i = 0; i < s_n; i++) {
        for (int j = i + 1; j < s_n; j++) {
            int ri = (s_list[i].kind == DTC_KIND_J1939) ? lamp_rank(s_list[i].lamps) : (s_list[i].pending ? 3 : 1);
            int rj = (s_list[j].kind == DTC_KIND_J1939) ? lamp_rank(s_list[j].lamps) : (s_list[j].pending ? 3 : 1);
            if (rj < ri || (rj == ri && s_list[j].oc > s_list[i].oc)) {
                dtc_entry_t t = s_list[i];
                s_list[i] = s_list[j];
                s_list[j] = t;
            }
        }
    }
}

static void fill_j1939_text(dtc_entry_t *e)
{
    const char *nm = dtc_spn_name(e->spn);
    const char *fmi = dtc_fmi_text(e->fmi);
    if (nm) {
        snprintf(e->title, sizeof(e->title), "%s — %s", nm, fmi);
    } else {
        snprintf(e->title, sizeof(e->title), "SPN %lu — %s", (unsigned long)e->spn, fmi);
    }
    snprintf(e->sub, sizeof(e->sub), "SPN %lu  FMI %u  count %u  %s  SA 0x%02X",
             (unsigned long)e->spn, e->fmi, e->oc, lamp_name(e->lamps), e->sa);
}

void dtc_init(void)
{
    s_n = 0;
}

void dtc_apply_dm1(uint8_t sa, const uint8_t *payload, uint16_t len)
{
    s_n = 0;
    if (len < 6) {
        return;
    }
    uint8_t lamps = payload[0];
    /* bytes 2+ are 4-byte DTCs; first DTC starts at offset 2 */
    for (uint16_t off = 2; off + 4 <= len && s_n < DTC_MAX; off += 4) {
        const uint8_t *d = payload + off;
        if (d[0] == 0xFF && d[1] == 0xFF && d[2] == 0xFF && d[3] == 0xFF) {
            continue;
        }
        uint32_t spn = decode_spn(d);
        uint8_t fmi = d[2] & 0x1F;
        uint8_t oc = d[3] & 0x7F;
        if (spn == 0 && fmi == 0 && oc == 0) {
            continue;
        }
        dtc_entry_t *e = &s_list[s_n++];
        memset(e, 0, sizeof(*e));
        e->kind = DTC_KIND_J1939;
        e->spn = spn;
        e->fmi = fmi;
        e->oc = oc;
        e->sa = sa;
        e->lamps = lamps;
        e->last_us = esp_timer_get_time();
        fill_j1939_text(e);
    }
    sort_list();
}

void dtc_apply_obd(uint16_t code, bool pending, bool mil)
{
    if (s_n >= DTC_MAX) {
        return;
    }
    for (int i = 0; i < s_n; i++) {
        if (s_list[i].kind == DTC_KIND_OBD && s_list[i].obd_code == code) {
            s_list[i].pending = pending;
            s_list[i].last_us = esp_timer_get_time();
            return;
        }
    }
    dtc_entry_t *e = &s_list[s_n++];
    memset(e, 0, sizeof(*e));
    e->kind = DTC_KIND_OBD;
    e->obd_code = code;
    e->pending = pending;
    e->lamps = mil ? 0xC0 : 0;
    e->last_us = esp_timer_get_time();
    dtc_format_obd(code, e->title, sizeof(e->title));
    snprintf(e->sub, sizeof(e->sub), "%s  %s", pending ? "Pending" : "Confirmed", mil ? "MIL on" : "MIL off");
    sort_list();
}

int dtc_count(void)
{
    return s_n;
}

const dtc_entry_t *dtc_get(int idx)
{
    if (idx < 0 || idx >= s_n) {
        return NULL;
    }
    return &s_list[idx];
}

bool dtc_bus_snapshot_stale(void)
{
    return signals_bus_lost();
}
