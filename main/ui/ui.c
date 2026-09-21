#include "ui.h"
#include "pins.h"
#include "signals.h"
#include "profile.h"
#include "dtc.h"
#include "j1939_tx.h"
#include "twai_port.h"
#include "sd_fs.h"
#include "proprietary.h"
#include "icons.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ui";
static SemaphoreHandle_t s_lv;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;

static lv_obj_t *s_tell[9];
static lv_obj_t *s_tell_plate[9];
static lv_obj_t *s_bus;
static lv_obj_t *s_arcs[6];
static lv_obj_t *s_arc_lbl[6];
static lv_obj_t *s_arc_name[6];
static lv_obj_t *s_dtc_list;
static lv_obj_t *s_dtc_empty;
static lv_obj_t *s_can_list;
static lv_obj_t *s_eng_rpm;
static lv_obj_t *s_eng_actual;
static lv_obj_t *s_eng_gate;
static lv_obj_t *s_eng_hold;
static lv_obj_t *s_profile_dd;
static lv_obj_t *s_tx_sw;
static lv_obj_t *s_obd_sw;
static lv_obj_t *s_sd_lbl;
static lv_obj_t *s_picker_win;
static bool s_on_engine;
static int s_picker_idx = -1;

typedef struct {
    signal_id_t sid;
    float min;
    float max;
    const char *label;
} gauge_cfg_t;

static gauge_cfg_t s_gauges[6] = {
    { SIG_RPM, 0, 3000, "RPM" },
    { SIG_COOLANT_C, 0, 120, "Coolant" },
    { SIG_OIL_KPA, 0, 600, "Oil kPa" },
    { SIG_VOLT, 8, 16, "Volt" },
    { SIG_SPEED_KPH, 0, 120, "Speed" },
    { SIG_FUEL_PCT, 0, 100, "Fuel/DEF" },
};

static const signal_id_t k_tell_sig[] = {
    SIG_MIL, SIG_RED_STOP, SIG_AMBER, SIG_PROTECT, SIG_DPF, SIG_HEST, SIG_DEF_PCT, SIG_WAIT_START, SIG_WIF
};

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2 + 1;
    int y2 = area->y2 + 1;
    esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2, y2, color_map);
    lv_disp_flush_ready(drv);
}

static void touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    uint16_t x[1], y[1];
    uint8_t n = 0;
    esp_lcd_touch_read_data(s_touch);
    bool pressed = esp_lcd_touch_get_coordinates(s_touch, x, y, NULL, &n, 1);
    if (pressed && n) {
        data->point.x = x[0];
        data->point.y = y[0];
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        if (s_on_engine) {
            j1939_tsc1_hold(false, 0);
        }
    }
}

static lv_color_t tell_color(signal_id_t id, float v, bool *on)
{
    *on = v >= 0.5f;
    if (!*on) {
        return lv_color_hex(0x8B93A0);
    }
    if (id == SIG_RED_STOP || id == SIG_MIL) {
        return lv_color_hex(0xFF5252);
    }
    if (id == SIG_HEST || id == SIG_AMBER || id == SIG_DPF) {
        return lv_color_hex(0xFFC107);
    }
    return lv_color_hex(0x4FC3F7);
}

static void refresh_telltale(void)
{
    for (int i = 0; i < 9; i++) {
        float v = 0;
        signal_quality_t q = SIG_Q_STALE;
        signals_get(k_tell_sig[i], &v, &q, NULL);
        if (k_tell_sig[i] == SIG_DEF_PCT) {
            v = (v > 0 && v < 10) ? 1.f : 0.f;
        }
        bool on = false;
        lv_color_t col = tell_color(k_tell_sig[i], v, &on);
        lv_obj_set_style_img_recolor(s_tell[i], col, 0);
        lv_obj_set_style_img_recolor_opa(s_tell[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_tell_plate[i], on ? col : lv_color_hex(0x2A3140), 0);
        lv_obj_set_style_bg_color(s_tell_plate[i], on ? lv_color_hex(0x1A1F28) : lv_color_hex(0x10141A), 0);
    }
    if (signals_bus_lost()) {
        lv_label_set_text(s_bus, "BUS LOST");
        lv_obj_set_style_text_color(s_bus, lv_color_hex(0xFF5252), 0);
    } else {
        lv_label_set_text(s_bus, "BUS OK");
        lv_obj_set_style_text_color(s_bus, lv_color_hex(0x69F0AE), 0);
    }
}

static void refresh_gauges(void)
{
    dash_profile_t *p = profile_get();
    for (int i = 0; i < 6; i++) {
        float v = 0;
        signal_quality_t q = SIG_Q_STALE;
        signals_get(s_gauges[i].sid, &v, &q, NULL);
        float show = v;
        if (!p->metric && s_gauges[i].sid == SIG_COOLANT_C) {
            show = v * 9.f / 5.f + 32.f;
        }
        if (!p->metric && s_gauges[i].sid == SIG_SPEED_KPH) {
            show = v * 0.621371f;
        }
        float span = s_gauges[i].max - s_gauges[i].min;
        int ang = 0;
        if (span > 0) {
            ang = (int)((show - s_gauges[i].min) * 270.f / span);
        }
        if (ang < 0) {
            ang = 0;
        }
        if (ang > 270) {
            ang = 270;
        }
        lv_arc_set_value(s_arcs[i], ang);
        char buf[32];
        snprintf(buf, sizeof(buf), q == SIG_Q_STALE ? "--" : "%.0f", show);
        lv_label_set_text(s_arc_lbl[i], buf);
        float t = span > 0 ? (show - s_gauges[i].min) / span : 0;
        if (t < 0) {
            t = 0;
        }
        bool low_bad = s_gauges[i].sid == SIG_FUEL_PCT || s_gauges[i].sid == SIG_DEF_PCT;
        lv_color_t col = lv_color_hex(0x4FC3F7);
        if (q == SIG_Q_STALE) {
            col = lv_color_hex(0x5C6570);
        } else if ((low_bad && t < 0.15f) || (!low_bad && t > 0.9f)) {
            col = lv_color_hex(0xFF5252);
        } else if ((low_bad && t < 0.30f) || (!low_bad && t > 0.75f)) {
            col = lv_color_hex(0xFFC107);
        } else if (s_gauges[i].sid == SIG_OIL_KPA) {
            col = lv_color_hex(0xFFB300);
        } else if (s_gauges[i].sid == SIG_VOLT || low_bad) {
            col = lv_color_hex(0x69F0AE);
        }
        lv_obj_set_style_arc_color(s_arcs[i], col, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s_arc_lbl[i], q == SIG_Q_STALE ? lv_color_hex(0x6B7280) : lv_color_hex(0xF5F7FA), 0);
        lv_label_set_text(s_arc_name[i], s_gauges[i].label);
    }
}

static void refresh_dtc(void)
{
    lv_obj_clean(s_dtc_list);
    int n = dtc_count();
    if (n == 0) {
        lv_label_set_text(s_dtc_empty, dtc_bus_snapshot_stale() ? "Bus lost — last faults shown" : "No active faults");
        lv_obj_clear_flag(s_dtc_empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_dtc_empty, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < n; i++) {
        const dtc_entry_t *e = dtc_get(i);
        lv_obj_t *btn = lv_list_add_btn(s_dtc_list, NULL, e->title);
        lv_obj_t *sub = lv_label_create(btn);
        lv_label_set_text(sub, e->sub);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(0xAAAAAA), 0);
    }
}

static void unmatched_click(lv_event_t *e);

static void refresh_canlog(void)
{
    lv_obj_clean(s_can_list);
    int n = twai_unmatched_count();
    for (int i = 0; i < n; i++) {
        uint32_t id, cnt;
        uint8_t dlc, data[8];
        twai_unmatched_get(i, &id, &cnt, &dlc, data);
        char line[96];
        snprintf(line, sizeof(line), "%08lX  x%lu  [%u]", (unsigned long)id, (unsigned long)cnt, dlc);
        lv_obj_t *row = lv_list_add_btn(s_can_list, NULL, line);
        lv_obj_add_event_cb(row, unmatched_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void refresh_engine(void)
{
    float rpm = 0;
    signals_get(SIG_RPM, &rpm, NULL, NULL);
    char b[48];
    snprintf(b, sizeof(b), "Actual %.0f rpm", rpm);
    lv_label_set_text(s_eng_actual, b);
    lv_label_set_text(s_eng_gate, j1939_tx_gate_reason());
}

static void apply_picker(signal_id_t sid)
{
    if (s_picker_idx < 0 || s_picker_idx >= 6) {
        return;
    }
    s_gauges[s_picker_idx].sid = sid;
    s_gauges[s_picker_idx].label = signal_name(sid);
    switch (sid) {
    case SIG_RPM:
        s_gauges[s_picker_idx].min = 0;
        s_gauges[s_picker_idx].max = 3000;
        break;
    case SIG_COOLANT_C:
    case SIG_OIL_C:
        s_gauges[s_picker_idx].min = 0;
        s_gauges[s_picker_idx].max = 140;
        break;
    case SIG_OIL_KPA:
        s_gauges[s_picker_idx].min = 0;
        s_gauges[s_picker_idx].max = 700;
        break;
    case SIG_VOLT:
        s_gauges[s_picker_idx].min = 8;
        s_gauges[s_picker_idx].max = 16;
        break;
    default:
        s_gauges[s_picker_idx].min = 0;
        s_gauges[s_picker_idx].max = 100;
        break;
    }
}

static void picker_cb(lv_event_t *e)
{
    signal_id_t sid = (signal_id_t)(intptr_t)lv_event_get_user_data(e);
    apply_picker(sid);
    if (s_picker_win) {
        lv_obj_del(s_picker_win);
        s_picker_win = NULL;
    }
}

static void open_picker(int idx)
{
    s_picker_idx = idx;
    if (s_picker_win) {
        lv_obj_del(s_picker_win);
    }
    s_picker_win = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_picker_win, 400, 400);
    lv_obj_center(s_picker_win);
    lv_obj_t *list = lv_list_create(s_picker_win);
    lv_obj_set_size(list, lv_pct(100), lv_pct(100));
    for (int i = 0; i < SIG_WAIT_START; i++) {
        lv_obj_t *b = lv_list_add_btn(list, NULL, signal_name((signal_id_t)i));
        lv_obj_add_event_cb(b, picker_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void gauge_long(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    open_picker(idx);
}

static void tab_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    uint32_t id = lv_tabview_get_tab_act(tv);
    s_on_engine = (id == 2);
    if (!s_on_engine) {
        j1939_tsc1_hold(false, 0);
    }
}

static void hold_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    uint16_t rpm = (uint16_t)lv_slider_get_value(s_eng_rpm);
    if (c == LV_EVENT_PRESSED) {
        j1939_tsc1_hold(true, rpm);
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        j1939_tsc1_hold(false, rpm);
    }
}

static void mbox_event(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    const char *txt = lv_msgbox_get_active_btn_text(mbox);
    if (txt && strstr(txt, "Start")) {
        j1939_regen_force(true);
        sd_fs_log_cmd("regen_force");
    }
    lv_msgbox_close(mbox);
}

static void regen_force_cb(lv_event_t *e)
{
    (void)e;
    static const char *btns[] = { "Cancel", "Start regen", "" };
    lv_obj_t *mbox = lv_msgbox_create(NULL, "Forced DPF regen",
                                      "Exhaust and DPF can be very hot. Keep people clear. Vehicle must be stationary.",
                                      btns, true);
    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, mbox_event, LV_EVENT_VALUE_CHANGED, NULL);
}

static void regen_inhibit_cb(lv_event_t *e)
{
    (void)e;
    j1939_regen_inhibit(true);
}

static void regen_cancel_cb(lv_event_t *e)
{
    (void)e;
    j1939_regen_cancel();
}

static void save_settings(void)
{
    dash_profile_t *p = profile_get();
    p->profile = (vehicle_profile_t)lv_dropdown_get_selected(s_profile_dd);
    p->j1939_tx_enable = lv_obj_has_state(s_tx_sw, LV_STATE_CHECKED);
    p->obd_requests = lv_obj_has_state(s_obd_sw, LV_STATE_CHECKED);
    profile_save();
    extern esp_err_t twai_port_restart(void);
    twai_port_restart();
    if (p->j1939_tx_enable) {
        j1939_tx_claim();
    }
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    save_settings();
}

static void sd_reload_cb(lv_event_t *e)
{
    (void)e;
    sd_fs_reload_database();
    lv_label_set_text(s_sd_lbl, sd_fs_status());
}

static void sd_log_cb(lv_event_t *e)
{
    (void)e;
    sd_fs_toggle_log();
    lv_label_set_text(s_sd_lbl, sd_fs_status());
}

static lv_obj_t *make_gauge(lv_obj_t *parent, int idx, int col, int row)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 252, 180);
    lv_obj_set_pos(card, 10 + col * 262, 54 + row * 186);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x12161C), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2A3140), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *arc = lv_arc_create(card);
    lv_obj_set_size(arc, 156, 156);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, 4);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 270);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x2A3038), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x4FC3F7), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_event_cb(arc, gauge_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)idx);

    lv_obj_t *val = lv_label_create(arc);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(0xF5F7FA), 0);
    lv_obj_align(val, LV_ALIGN_CENTER, 0, -8);
    lv_obj_t *nm = lv_label_create(arc);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(nm, lv_color_hex(0xC5CAD3), 0);
    lv_obj_align(nm, LV_ALIGN_CENTER, 0, 24);
    s_arcs[idx] = arc;
    s_arc_lbl[idx] = val;
    s_arc_name[idx] = nm;
    return arc;
}

static lv_obj_t *s_prop_panel;
static lv_obj_t *s_prop_kb;
static lv_obj_t *s_prop_status;
static lv_obj_t *s_ta_id;
static lv_obj_t *s_ta_dlc;
static lv_obj_t *s_ta_mux_start;
static lv_obj_t *s_ta_mux_len;
static lv_obj_t *s_ta_mux_val;
static lv_obj_t *s_ta_start;
static lv_obj_t *s_ta_len;
static lv_obj_t *s_ta_scale;
static lv_obj_t *s_ta_offset;
static lv_obj_t *s_sw_ext;
static lv_obj_t *s_sw_mux;
static lv_obj_t *s_sw_signed;
static lv_obj_t *s_dd_sig;
static lv_obj_t *s_dd_endian;
static char s_sig_opts[384];

static void prop_lbl(lv_obj_t *parent, const char *text, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y + 8);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFEB3B), 0);
}

static void prop_ta_focus(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    int hex = (int)(intptr_t)lv_obj_get_user_data(ta);
    lv_keyboard_set_mode(s_prop_kb, hex ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(s_prop_kb, ta);
}

static lv_obj_t *prop_ta(lv_obj_t *parent, int x, int y, int w, const char *txt, bool hex)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_pos(ta, x, y);
    lv_obj_set_size(ta, w, 36);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, hex ? 8 : 12);
    lv_textarea_set_accepted_chars(ta, hex ? "0123456789abcdefABCDEF" : "0123456789.-");
    lv_textarea_set_text(ta, txt);
    lv_obj_set_user_data(ta, (void *)(intptr_t)(hex ? 1 : 0));
    lv_obj_add_event_cb(ta, prop_ta_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, prop_ta_focus, LV_EVENT_CLICKED, NULL);
    return ta;
}

static void prop_reset_fields(void)
{
    lv_textarea_set_text(s_ta_id, "");
    lv_textarea_set_text(s_ta_dlc, "8");
    lv_textarea_set_text(s_ta_mux_start, "0");
    lv_textarea_set_text(s_ta_mux_len, "8");
    lv_textarea_set_text(s_ta_mux_val, "1");
    lv_textarea_set_text(s_ta_start, "16");
    lv_textarea_set_text(s_ta_len, "16");
    lv_textarea_set_text(s_ta_scale, "0.125");
    lv_textarea_set_text(s_ta_offset, "0");
    lv_obj_add_state(s_sw_ext, LV_STATE_CHECKED);
    lv_obj_clear_state(s_sw_mux, LV_STATE_CHECKED);
    lv_obj_clear_state(s_sw_signed, LV_STATE_CHECKED);
    lv_dropdown_set_selected(s_dd_sig, SIG_RPM);
    lv_dropdown_set_selected(s_dd_endian, 0);
    lv_label_set_text(s_prop_status, "Tap a field, then use the keyboard");
    lv_keyboard_set_textarea(s_prop_kb, s_ta_id);
    lv_keyboard_set_mode(s_prop_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
}

static void prop_open(uint32_t id, bool have_id)
{
    if (!have_id) {
        prop_reset_fields();
    } else {
        char b[16];
        snprintf(b, sizeof(b), "%lX", (unsigned long)id);
        lv_textarea_set_text(s_ta_id, b);
        if (id <= 0x7FF) {
            lv_obj_clear_state(s_sw_ext, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(s_sw_ext, LV_STATE_CHECKED);
        }
        lv_label_set_text(s_prop_status, "ID filled from the CAN list");
        lv_keyboard_set_textarea(s_prop_kb, s_ta_id);
    }
    lv_obj_clear_flag(s_prop_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_prop_panel);
}

static bool read_u32(lv_obj_t *ta, uint32_t *out, int base)
{
    const char *t = lv_textarea_get_text(ta);
    if (!t || !t[0]) {
        return false;
    }
    char *end = NULL;
    unsigned long v = strtoul(t, &end, base);
    if (end == t) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool read_f(lv_obj_t *ta, float *out)
{
    const char *t = lv_textarea_get_text(ta);
    if (!t || !t[0]) {
        return false;
    }
    char *end = NULL;
    float v = strtof(t, &end);
    if (end == t) {
        return false;
    }
    *out = v;
    return true;
}

static void prop_save_cb(lv_event_t *e)
{
    (void)e;
    proprietary_entry_t ent;
    memset(&ent, 0, sizeof(ent));
    if (!read_u32(s_ta_id, &ent.id, 16)) {
        lv_label_set_text(s_prop_status, "Enter a hex CAN ID");
        return;
    }
    uint32_t dlc = 8, mux_start = 0, mux_len = 8, mux_val = 0, start = 0, length = 0;
    if (!read_u32(s_ta_dlc, &dlc, 10) || dlc > 8) {
        lv_label_set_text(s_prop_status, "DLC must be 0 to 8");
        return;
    }
    bool mux = lv_obj_has_state(s_sw_mux, LV_STATE_CHECKED);
    if (mux) {
        if (!read_u32(s_ta_mux_start, &mux_start, 10) || !read_u32(s_ta_mux_len, &mux_len, 10) ||
            !read_u32(s_ta_mux_val, &mux_val, 10) || mux_len == 0 || mux_len > 32 || mux_start > 63) {
            lv_label_set_text(s_prop_status, "Mux start, length, and value");
            return;
        }
    }
    if (!read_u32(s_ta_start, &start, 10) || !read_u32(s_ta_len, &length, 10) || length == 0 || length > 32 || start > 63) {
        lv_label_set_text(s_prop_status, "Start bit 0-63, length 1-32");
        return;
    }
    if (!read_f(s_ta_scale, &ent.scale) || !read_f(s_ta_offset, &ent.offset)) {
        lv_label_set_text(s_prop_status, "Enter scale and offset");
        return;
    }
    ent.id_mask = 0x1FFFFFFF;
    ent.extended = lv_obj_has_state(s_sw_ext, LV_STATE_CHECKED);
    ent.dlc = (uint8_t)dlc;
    ent.has_mux = mux;
    ent.mux_start = (uint16_t)mux_start;
    ent.mux_len = (uint8_t)mux_len;
    ent.mux_val = mux_val;
    ent.sid = (signal_id_t)lv_dropdown_get_selected(s_dd_sig);
    ent.start_bit = (uint16_t)start;
    ent.length = (uint8_t)length;
    ent.intel = lv_dropdown_get_selected(s_dd_endian) == 0;
    ent.is_signed = lv_obj_has_state(s_sw_signed, LV_STATE_CHECKED);
    esp_err_t err = proprietary_add(&ent);
    if (err != ESP_OK) {
        lv_label_set_text(s_prop_status, err == ESP_ERR_NO_MEM ? "Table full (24 messages, 8 signals)" : "Check the fields");
        return;
    }
    err = proprietary_save_nvs();
    char *json = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json) {
        json = malloc(16384);
    }
    bool on_sd = false;
    if (json && proprietary_export_json(json, 16384, NULL) == ESP_OK && sd_fs_mounted()) {
        on_sd = sd_fs_save_proprietary(json) == ESP_OK;
    }
    free(json);
    if (err != ESP_OK) {
        lv_label_set_text(s_prop_status, "Saved for this session. Storage write failed");
        return;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), on_sd ? "Saved (%d) on device and SD" : "Saved (%d) on device", proprietary_message_count());
    lv_label_set_text(s_prop_status, msg);
}

static void prop_cancel_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_prop_panel, LV_OBJ_FLAG_HIDDEN);
}

static void add_msg_cb(lv_event_t *e)
{
    (void)e;
    prop_open(0, false);
}

static void unmatched_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    uint32_t id = 0;
    twai_unmatched_get(idx, &id, NULL, NULL, NULL);
    prop_open(id, true);
}

static void build_prop_editor(void)
{
    if (s_sig_opts[0] == 0) {
        size_t n = 0;
        for (int i = 0; i < SIG_COUNT; i++) {
            int w = snprintf(s_sig_opts + n, sizeof(s_sig_opts) - n, "%s%s", i ? "\n" : "", signal_name((signal_id_t)i));
            if (w < 0 || (size_t)w >= sizeof(s_sig_opts) - n) {
                break;
            }
            n += (size_t)w;
        }
    }
    s_prop_panel = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_prop_panel, 800, 480);
    lv_obj_set_pos(s_prop_panel, 0, 0);
    lv_obj_set_style_bg_color(s_prop_panel, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(s_prop_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_prop_panel, 0, 0);
    lv_obj_set_style_pad_all(s_prop_panel, 0, 0);
    lv_obj_set_style_radius(s_prop_panel, 0, 0);
    lv_obj_clear_flag(s_prop_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_prop_panel, LV_OBJ_FLAG_HIDDEN);

    prop_lbl(s_prop_panel, "Proprietary message", 8, 0);
    s_prop_status = lv_label_create(s_prop_panel);
    lv_obj_set_pos(s_prop_status, 230, 8);
    lv_obj_set_width(s_prop_status, 550);
    lv_label_set_long_mode(s_prop_status, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_prop_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_prop_status, lv_color_hex(0xFFEB3B), 0);

    prop_lbl(s_prop_panel, "ID", 8, 40);
    s_ta_id = prop_ta(s_prop_panel, 40, 40, 150, "", true);
    prop_lbl(s_prop_panel, "Ext", 210, 40);
    s_sw_ext = lv_switch_create(s_prop_panel);
    lv_obj_set_pos(s_sw_ext, 250, 42);
    lv_obj_add_state(s_sw_ext, LV_STATE_CHECKED);
    prop_lbl(s_prop_panel, "DLC", 340, 40);
    s_ta_dlc = prop_ta(s_prop_panel, 390, 40, 70, "8", false);

    prop_lbl(s_prop_panel, "Mux", 8, 86);
    s_sw_mux = lv_switch_create(s_prop_panel);
    lv_obj_set_pos(s_sw_mux, 60, 88);
    prop_lbl(s_prop_panel, "start", 140, 86);
    s_ta_mux_start = prop_ta(s_prop_panel, 200, 86, 70, "0", false);
    prop_lbl(s_prop_panel, "len", 290, 86);
    s_ta_mux_len = prop_ta(s_prop_panel, 330, 86, 70, "8", false);
    prop_lbl(s_prop_panel, "value", 420, 86);
    s_ta_mux_val = prop_ta(s_prop_panel, 480, 86, 90, "1", false);

    prop_lbl(s_prop_panel, "Signal", 8, 132);
    s_dd_sig = lv_dropdown_create(s_prop_panel);
    lv_dropdown_set_options(s_dd_sig, s_sig_opts);
    lv_obj_set_pos(s_dd_sig, 80, 132);
    lv_obj_set_width(s_dd_sig, 180);
    prop_lbl(s_prop_panel, "Order", 280, 132);
    s_dd_endian = lv_dropdown_create(s_prop_panel);
    lv_dropdown_set_options(s_dd_endian, "intel\nmotorola");
    lv_obj_set_pos(s_dd_endian, 350, 132);
    lv_obj_set_width(s_dd_endian, 150);
    prop_lbl(s_prop_panel, "Signed", 520, 132);
    s_sw_signed = lv_switch_create(s_prop_panel);
    lv_obj_set_pos(s_sw_signed, 590, 134);

    prop_lbl(s_prop_panel, "Start", 8, 178);
    s_ta_start = prop_ta(s_prop_panel, 70, 178, 80, "16", false);
    prop_lbl(s_prop_panel, "Length", 170, 178);
    s_ta_len = prop_ta(s_prop_panel, 250, 178, 80, "16", false);
    prop_lbl(s_prop_panel, "Scale", 350, 178);
    s_ta_scale = prop_ta(s_prop_panel, 420, 178, 100, "0.125", false);
    prop_lbl(s_prop_panel, "Offset", 540, 178);
    s_ta_offset = prop_ta(s_prop_panel, 610, 178, 100, "0", false);

    lv_obj_t *save = lv_btn_create(s_prop_panel);
    lv_obj_set_pos(save, 8, 230);
    lv_obj_set_size(save, 140, 44);
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, "Save");
    lv_obj_center(sl);
    lv_obj_add_event_cb(save, prop_save_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel = lv_btn_create(s_prop_panel);
    lv_obj_set_pos(cancel, 160, 230);
    lv_obj_set_size(cancel, 140, 44);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, prop_cancel_cb, LV_EVENT_CLICKED, NULL);

    s_prop_kb = lv_keyboard_create(s_prop_panel);
    lv_obj_set_size(s_prop_kb, 800, 190);
    lv_obj_align(s_prop_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(s_prop_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_prop_kb, s_ta_id);
}

static lv_obj_t *s_live_panel;
static lv_obj_t *s_live_list;
static lv_obj_t *s_live_status;
static lv_obj_t *s_live_pause_lbl;
static bool s_live_paused;
static uint32_t s_live_prev_id[24];
static uint32_t s_live_prev_cnt[24];
static int64_t s_live_prev_us;

static void live_row_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    uint32_t id = 0;
    twai_live_get(idx, &id, NULL, NULL, NULL, NULL);
    s_live_paused = true;
    lv_label_set_text(s_live_pause_lbl, "Resume");
    prop_open(id, true);
}

static void refresh_live(void)
{
    if (!s_live_panel || lv_obj_has_flag(s_live_panel, LV_OBJ_FLAG_HIDDEN) || s_live_paused) {
        return;
    }
    int64_t now = esp_timer_get_time();
    int64_t dt = now - s_live_prev_us;
    if (s_live_prev_us != 0 && dt < 200000) {
        return;
    }
    s_live_prev_us = now;
    int n = twai_live_count();
    char head[80];
    snprintf(head, sizeof(head), "%d IDs    %lu kbit/s    %s", n,
             (unsigned long)(profile_bitrate() / 1000),
             profile_listen_only() ? "listen only" : "normal");
    lv_label_set_text(s_live_status, head);
    lv_obj_clean(s_live_list);
    for (int i = 0; i < n && i < 24; i++) {
        uint32_t id = 0, cnt = 0;
        uint8_t dlc = 0, data[8] = { 0 };
        bool extd = false;
        twai_live_get(i, &id, &cnt, &dlc, data, &extd);
        uint32_t delta = 0;
        if (s_live_prev_id[i] == id && cnt >= s_live_prev_cnt[i]) {
            delta = cnt - s_live_prev_cnt[i];
        }
        s_live_prev_id[i] = id;
        s_live_prev_cnt[i] = cnt;
        unsigned rate = (dt > 0) ? (unsigned)((delta * 1000000ULL) / (uint64_t)dt) : 0;
        char line[96];
        int pos = snprintf(line, sizeof(line), extd ? "%08lX  %u  " : "     %03lX  %u  ", (unsigned long)id, dlc);
        for (uint8_t b = 0; b < dlc && b < 8 && pos < (int)sizeof(line) - 8; b++) {
            pos += snprintf(line + pos, sizeof(line) - (size_t)pos, "%02X ", data[b]);
        }
        snprintf(line + pos, sizeof(line) - (size_t)pos, "%u/s", rate);
        lv_obj_t *row = lv_list_add_btn(s_live_list, NULL, line);
        lv_obj_set_style_text_font(row, &lv_font_montserrat_14, 0);
        lv_obj_add_event_cb(row, live_row_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void live_open_cb(lv_event_t *e)
{
    (void)e;
    memset(s_live_prev_id, 0, sizeof(s_live_prev_id));
    memset(s_live_prev_cnt, 0, sizeof(s_live_prev_cnt));
    s_live_prev_us = 0;
    s_live_paused = false;
    lv_label_set_text(s_live_pause_lbl, "Pause");
    twai_live_clear();
    twai_live_set_enabled(true);
    lv_obj_clear_flag(s_live_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_live_panel);
}

static void live_close_cb(lv_event_t *e)
{
    (void)e;
    twai_live_set_enabled(false);
    lv_obj_add_flag(s_live_panel, LV_OBJ_FLAG_HIDDEN);
}

static void live_pause_cb(lv_event_t *e)
{
    (void)e;
    s_live_paused = !s_live_paused;
    lv_label_set_text(s_live_pause_lbl, s_live_paused ? "Resume" : "Pause");
    if (!s_live_paused) {
        s_live_prev_us = 0;
    }
}

static void live_clear_cb(lv_event_t *e)
{
    (void)e;
    twai_live_clear();
    memset(s_live_prev_id, 0, sizeof(s_live_prev_id));
    memset(s_live_prev_cnt, 0, sizeof(s_live_prev_cnt));
    s_live_prev_us = 0;
    lv_obj_clean(s_live_list);
}

static void build_live_monitor(void)
{
    s_live_panel = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_live_panel, 800, 480);
    lv_obj_set_pos(s_live_panel, 0, 0);
    lv_obj_set_style_bg_color(s_live_panel, lv_color_hex(0x07090B), 0);
    lv_obj_set_style_bg_opa(s_live_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_live_panel, 0, 0);
    lv_obj_set_style_radius(s_live_panel, 0, 0);
    lv_obj_set_style_pad_all(s_live_panel, 0, 0);
    lv_obj_clear_flag(s_live_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_live_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(s_live_panel);
    lv_label_set_text(title, "Live CAN");
    lv_obj_set_pos(title, 12, 10);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFEB3B), 0);
    s_live_status = lv_label_create(s_live_panel);
    lv_obj_set_pos(s_live_status, 150, 14);
    lv_obj_set_style_text_font(s_live_status, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_live_status, lv_color_hex(0xFFEB3B), 0);
    lv_label_set_text(s_live_status, "waiting");

    lv_obj_t *pause = lv_btn_create(s_live_panel);
    lv_obj_set_pos(pause, 470, 4);
    lv_obj_set_size(pause, 100, 40);
    s_live_pause_lbl = lv_label_create(pause);
    lv_label_set_text(s_live_pause_lbl, "Pause");
    lv_obj_center(s_live_pause_lbl);
    lv_obj_add_event_cb(pause, live_pause_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clr = lv_btn_create(s_live_panel);
    lv_obj_set_pos(clr, 578, 4);
    lv_obj_set_size(clr, 100, 40);
    lv_obj_t *clrl = lv_label_create(clr);
    lv_label_set_text(clrl, "Clear");
    lv_obj_center(clrl);
    lv_obj_add_event_cb(clr, live_clear_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close = lv_btn_create(s_live_panel);
    lv_obj_set_pos(close, 686, 4);
    lv_obj_set_size(close, 100, 40);
    lv_obj_t *closel = lv_label_create(close);
    lv_label_set_text(closel, "Close");
    lv_obj_center(closel);
    lv_obj_add_event_cb(close, live_close_cb, LV_EVENT_CLICKED, NULL);

    s_live_list = lv_list_create(s_live_panel);
    lv_obj_set_size(s_live_list, 776, 410);
    lv_obj_set_pos(s_live_list, 12, 52);
    lv_obj_set_style_text_font(s_live_list, &lv_font_montserrat_14, 0);
}

static void build_ui(void)
{
    lv_obj_t *tv = lv_tabview_create(lv_scr_act(), LV_DIR_BOTTOM, 52);
    lv_obj_add_event_cb(tv, tab_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *dash = lv_tabview_add_tab(tv, "Dash");
    lv_obj_t *dtc = lv_tabview_add_tab(tv, "DTC");
    lv_obj_t *eng = lv_tabview_add_tab(tv, "Engine");
    lv_obj_t *can = lv_tabview_add_tab(tv, "CAN");
    lv_obj_t *set = lv_tabview_add_tab(tv, "Set");

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x07090B), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(lv_scr_act(), lv_color_hex(0xE8EAED), 0);
    lv_obj_set_style_text_font(lv_scr_act(), &lv_font_montserrat_16, 0);
    lv_obj_t *pages[] = { dash, dtc, eng, can, set };
    for (int i = 0; i < 5; i++) {
        lv_obj_set_style_bg_color(pages[i], lv_color_hex(0x07090B), 0);
        lv_obj_set_style_bg_opa(pages[i], LV_OPA_COVER, 0);
        lv_obj_set_style_text_font(pages[i], &lv_font_montserrat_16, 0);
    }
    lv_obj_t *btns = lv_tabview_get_tab_btns(tv);
    lv_obj_set_style_bg_color(btns, lv_color_hex(0x0C0E12), 0);
    lv_obj_set_style_text_font(btns, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_obj_set_style_text_color(btns, lv_color_hex(0x9AA0A6), LV_PART_ITEMS);
    lv_obj_set_style_text_color(btns, lv_color_hex(0xFFEB3B), LV_PART_ITEMS | LV_STATE_CHECKED);

    for (int i = 0; i < 9; i++) {
        s_tell_plate[i] = lv_obj_create(dash);
        lv_obj_set_size(s_tell_plate[i], 48, 44);
        lv_obj_set_pos(s_tell_plate[i], 8 + i * 76, 2);
        lv_obj_set_style_bg_color(s_tell_plate[i], lv_color_hex(0x10141A), 0);
        lv_obj_set_style_bg_opa(s_tell_plate[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_tell_plate[i], lv_color_hex(0x2A3140), 0);
        lv_obj_set_style_border_width(s_tell_plate[i], 1, 0);
        lv_obj_set_style_radius(s_tell_plate[i], 8, 0);
        lv_obj_set_style_pad_all(s_tell_plate[i], 0, 0);
        lv_obj_clear_flag(s_tell_plate[i], LV_OBJ_FLAG_SCROLLABLE);
        s_tell[i] = lv_img_create(s_tell_plate[i]);
        lv_img_set_src(s_tell[i], telltale_icons[i]);
        lv_obj_center(s_tell[i]);
        lv_obj_set_style_img_recolor(s_tell[i], lv_color_hex(0x8B93A0), 0);
        lv_obj_set_style_img_recolor_opa(s_tell[i], LV_OPA_COVER, 0);
    }
    s_bus = lv_label_create(dash);
    lv_obj_set_pos(s_bus, 692, 12);
    lv_obj_set_style_text_font(s_bus, &lv_font_montserrat_16, 0);

    make_gauge(dash, 0, 0, 0);
    make_gauge(dash, 1, 1, 0);
    make_gauge(dash, 2, 2, 0);
    make_gauge(dash, 3, 0, 1);
    make_gauge(dash, 4, 1, 1);
    make_gauge(dash, 5, 2, 1);

    s_dtc_empty = lv_label_create(dtc);
    lv_label_set_text(s_dtc_empty, "No active faults");
    lv_obj_center(s_dtc_empty);
    s_dtc_list = lv_list_create(dtc);
    lv_obj_set_size(s_dtc_list, 760, 360);

    lv_obj_t *hint = lv_label_create(eng);
    lv_label_set_text(hint, "TSC1 hold-to-run (service RPM). Regen needs TX enable + address claim.");
    lv_obj_set_pos(hint, 12, 8);
    s_eng_rpm = lv_slider_create(eng);
    lv_obj_set_size(s_eng_rpm, 500, 20);
    lv_obj_set_pos(s_eng_rpm, 12, 50);
    lv_slider_set_range(s_eng_rpm, 600, 1200);
    lv_slider_set_value(s_eng_rpm, 800, LV_ANIM_OFF);
    s_eng_hold = lv_btn_create(eng);
    lv_obj_set_size(s_eng_hold, 180, 56);
    lv_obj_set_pos(s_eng_hold, 540, 32);
    lv_obj_t *ht = lv_label_create(s_eng_hold);
    lv_label_set_text(ht, "HOLD TSC1");
    lv_obj_center(ht);
    lv_obj_add_event_cb(s_eng_hold, hold_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_eng_hold, hold_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_eng_hold, hold_cb, LV_EVENT_PRESS_LOST, NULL);
    s_eng_actual = lv_label_create(eng);
    lv_obj_set_pos(s_eng_actual, 12, 90);
    s_eng_gate = lv_label_create(eng);
    lv_obj_set_pos(s_eng_gate, 12, 120);

    lv_obj_t *bf = lv_btn_create(eng);
    lv_obj_set_pos(bf, 12, 170);
    lv_obj_t *l1 = lv_label_create(bf);
    lv_label_set_text(l1, "Force regen");
    lv_obj_add_event_cb(bf, regen_force_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bi = lv_btn_create(eng);
    lv_obj_set_pos(bi, 180, 170);
    lv_obj_t *l2 = lv_label_create(bi);
    lv_label_set_text(l2, "Inhibit");
    lv_obj_add_event_cb(bi, regen_inhibit_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bc = lv_btn_create(eng);
    lv_obj_set_pos(bc, 300, 170);
    lv_obj_t *l3 = lv_label_create(bc);
    lv_label_set_text(l3, "Cancel");
    lv_obj_add_event_cb(bc, regen_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clbl = lv_label_create(can);
    lv_label_set_text(clbl, "Unmatched IDs");
    lv_obj_set_pos(clbl, 12, 8);
    lv_obj_t *liveb = lv_btn_create(can);
    lv_obj_set_pos(liveb, 340, 0);
    lv_obj_set_size(liveb, 200, 40);
    lv_obj_t *livel = lv_label_create(liveb);
    lv_label_set_text(livel, "Live monitor");
    lv_obj_center(livel);
    lv_obj_add_event_cb(liveb, live_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *addb = lv_btn_create(can);
    lv_obj_set_pos(addb, 560, 0);
    lv_obj_set_size(addb, 200, 40);
    lv_obj_t *addl = lv_label_create(addb);
    lv_label_set_text(addl, "Add message");
    lv_obj_center(addl);
    lv_obj_add_event_cb(addb, add_msg_cb, LV_EVENT_CLICKED, NULL);
    s_can_list = lv_list_create(can);
    lv_obj_set_size(s_can_list, 760, 340);
    lv_obj_set_pos(s_can_list, 8, 48);

    lv_obj_t *pd = lv_label_create(set);
    lv_label_set_text(pd, "Profile");
    lv_obj_set_pos(pd, 12, 12);
    s_profile_dd = lv_dropdown_create(set);
    lv_dropdown_set_options(s_profile_dd, "J1939 250k\nJ1939 500k\nOBD-II 500k\nJ1939+OEM 250k\nProprietary");
    lv_obj_set_pos(s_profile_dd, 12, 40);
    lv_dropdown_set_selected(s_profile_dd, profile_get()->profile);

    s_tx_sw = lv_switch_create(set);
    lv_obj_set_pos(s_tx_sw, 12, 100);
    if (profile_get()->j1939_tx_enable) {
        lv_obj_add_state(s_tx_sw, LV_STATE_CHECKED);
    }
    lv_obj_t *txl = lv_label_create(set);
    lv_label_set_text(txl, "Enable J1939 transmit");
    lv_obj_set_pos(txl, 72, 108);

    s_obd_sw = lv_switch_create(set);
    lv_obj_set_pos(s_obd_sw, 12, 150);
    if (profile_get()->obd_requests) {
        lv_obj_add_state(s_obd_sw, LV_STATE_CHECKED);
    }
    lv_obj_t *ol = lv_label_create(set);
    lv_label_set_text(ol, "Enable OBD requests");
    lv_obj_set_pos(ol, 72, 158);

    lv_obj_t *sv = lv_btn_create(set);
    lv_obj_set_pos(sv, 12, 200);
    lv_obj_t *svl = lv_label_create(sv);
    lv_label_set_text(svl, "Save & restart CAN");
    lv_obj_add_event_cb(sv, save_cb, LV_EVENT_CLICKED, NULL);

    s_sd_lbl = lv_label_create(set);
    lv_obj_set_pos(s_sd_lbl, 12, 260);
    lv_label_set_text(s_sd_lbl, "SD: not mounted");
    lv_obj_t *sdb = lv_btn_create(set);
    lv_obj_set_pos(sdb, 12, 290);
    lv_obj_t *sdl = lv_label_create(sdb);
    lv_label_set_text(sdl, "SD reload DB");
    lv_obj_add_event_cb(sdb, sd_reload_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sdt = lv_btn_create(set);
    lv_obj_set_pos(sdt, 180, 290);
    lv_obj_t *sdtl = lv_label_create(sdt);
    lv_label_set_text(sdtl, "Toggle log");
    lv_obj_add_event_cb(sdt, sd_log_cb, LV_EVENT_CLICKED, NULL);
    build_prop_editor();
    build_live_monitor();
}

static void lvgl_task(void *arg)
{
    (void)arg;
    uint32_t n = 0;
    while (1) {
        if (xSemaphoreTake(s_lv, pdMS_TO_TICKS(50)) == pdTRUE) {
            lv_timer_handler();
            if ((n++ % 5) == 0) {
                refresh_telltale();
                refresh_gauges();
                refresh_dtc();
                refresh_engine();
                if ((n % 25) == 0) {
                    refresh_canlog();
                }
                refresh_live();
            }
            xSemaphoreGive(s_lv);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t ui_start(esp_lcd_panel_handle_t panel, esp_lcd_touch_handle_t touch)
{
    s_panel = panel;
    s_touch = touch;
    s_lv = xSemaphoreCreateMutex();
    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    size_t px = BOARD_LCD_H_RES * 40;
    lv_color_t *b1 = heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    lv_color_t *b2 = heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!b1 || !b2) {
        b1 = heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
        b2 = heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    }
    lv_disp_draw_buf_init(&draw_buf, b1, b2, px);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = BOARD_LCD_H_RES;
    disp_drv.ver_res = BOARD_LCD_V_RES;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_cb;
    lv_indev_drv_register(&indev_drv);

    build_ui();
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 16384, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "UI started");
    return ESP_OK;
}

void ui_tick(void)
{
}
