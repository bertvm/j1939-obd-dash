#include "proprietary.h"
#include "signals.h"
#include "twai_port.h"

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "prop";

#define PROP_MAX_MSG 24
#define PROP_MAX_SIG 8
#define PROP_MAX_TELL 16

typedef struct {
    signal_id_t sid;
    uint16_t start_bit;
    uint8_t length;
    bool intel;
    bool is_signed;
    float scale;
    float offset;
} prop_sig_t;

typedef struct {
    uint32_t id;
    uint32_t id_mask;
    bool ext;
    uint8_t dlc;
    bool has_mux;
    uint16_t mux_start;
    uint8_t mux_len;
    uint32_t mux_val;
    int nsig;
    prop_sig_t sig[PROP_MAX_SIG];
} prop_msg_t;

typedef struct {
    signal_id_t from;
    signal_id_t icon;
    float on_value;
} prop_tell_t;

static prop_msg_t s_msg[PROP_MAX_MSG];
static int s_nmsg;
static prop_tell_t s_tell[PROP_MAX_TELL];
static int s_ntell;

static uint64_t extract_bits(const uint8_t *data, uint8_t dlc, uint16_t start, uint8_t len, bool intel)
{
    uint64_t packed = 0;
    int nbytes = dlc > 8 ? 8 : dlc;
    if (intel) {
        for (int i = nbytes - 1; i >= 0; i--) {
            packed = (packed << 8) | data[i];
        }
        packed >>= start;
    } else {
        for (int i = 0; i < nbytes; i++) {
            packed = (packed << 8) | data[i];
        }
        int total = nbytes * 8;
        int shift = total - start - len;
        if (shift < 0) {
            shift = 0;
        }
        packed >>= shift;
    }
    uint64_t mask = (len >= 64) ? ~0ULL : ((1ULL << len) - 1ULL);
    return packed & mask;
}

static signal_id_t sid_from_name(const char *n)
{
    for (int i = 0; i < SIG_COUNT; i++) {
        if (strcmp(signal_name((signal_id_t)i), n) == 0) {
            return (signal_id_t)i;
        }
    }
    if (strcmp(n, "coolant_c") == 0) {
        return SIG_COOLANT_C;
    }
    if (strcmp(n, "oil_kpa") == 0) {
        return SIG_OIL_KPA;
    }
    return SIG_COUNT;
}

int proprietary_message_count(void)
{
    return s_nmsg;
}

bool proprietary_on_frame(const can_frame_t *msg)
{
    bool matched = false;
    for (int i = 0; i < s_nmsg; i++) {
        prop_msg_t *m = &s_msg[i];
        if (m->ext != (bool)msg->extd) {
            continue;
        }
        if ((msg->identifier & m->id_mask) != (m->id & m->id_mask)) {
            continue;
        }
        if (m->dlc && msg->data_length_code < m->dlc) {
            continue;
        }
        if (m->has_mux) {
            uint64_t mux = extract_bits(msg->data, msg->data_length_code, m->mux_start, m->mux_len, true);
            if (mux != m->mux_val) {
                continue;
            }
        }
        matched = true;
        for (int s = 0; s < m->nsig; s++) {
            prop_sig_t *ps = &m->sig[s];
            uint64_t raw = extract_bits(msg->data, msg->data_length_code, ps->start_bit, ps->length, ps->intel);
            double v = (double)raw;
            if (ps->is_signed && ps->length < 64) {
                uint64_t sign = 1ULL << (ps->length - 1);
                if (raw & sign) {
                    v = (double)((int64_t)raw - (int64_t)(sign << 1));
                }
            }
            v = v * ps->scale + ps->offset;
            signals_set_override(ps->sid, (float)v, SIG_SRC_PROP);
        }
    }
    for (int t = 0; t < s_ntell; t++) {
        float v;
        if (signals_get(s_tell[t].from, &v, NULL, NULL) && v == s_tell[t].on_value) {
            signals_set_override(s_tell[t].icon, 1.f, SIG_SRC_PROP);
        }
    }
    if (!matched) {
        twai_unmatched_note(msg->identifier, msg->extd, msg->data, msg->data_length_code);
    }
    return matched;
}

static uint32_t parse_hexu32(cJSON *j, uint32_t def)
{
    if (!j) {
        return def;
    }
    if (cJSON_IsString(j)) {
        return (uint32_t)strtoul(j->valuestring, NULL, 0);
    }
    if (cJSON_IsNumber(j)) {
        return (uint32_t)j->valuedouble;
    }
    return def;
}

esp_err_t proprietary_load_json(const char *json, size_t len)
{
    (void)len;
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return ESP_FAIL;
    }
    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    if (!cJSON_IsArray(messages)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    s_nmsg = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, messages) {
        if (s_nmsg >= PROP_MAX_MSG) {
            break;
        }
        prop_msg_t *out = &s_msg[s_nmsg];
        memset(out, 0, sizeof(*out));
        out->id = parse_hexu32(cJSON_GetObjectItem(m, "id"), 0);
        out->id_mask = parse_hexu32(cJSON_GetObjectItem(m, "id_mask"), 0x1FFFFFFF);
        cJSON *ext = cJSON_GetObjectItem(m, "extended");
        out->ext = ext ? cJSON_IsTrue(ext) : true;
        cJSON *dlc = cJSON_GetObjectItem(m, "dlc");
        out->dlc = dlc ? (uint8_t)dlc->valuedouble : 0;
        cJSON *mux = cJSON_GetObjectItem(m, "mux");
        if (mux) {
            out->has_mux = true;
            out->mux_start = (uint16_t)cJSON_GetObjectItem(mux, "start_bit")->valuedouble;
            out->mux_len = (uint8_t)cJSON_GetObjectItem(mux, "length")->valuedouble;
            out->mux_val = (uint32_t)cJSON_GetObjectItem(mux, "value")->valuedouble;
        }
        cJSON *sigs = cJSON_GetObjectItem(m, "signals");
        if (cJSON_IsArray(sigs)) {
            cJSON *s;
            cJSON_ArrayForEach(s, sigs) {
                if (out->nsig >= PROP_MAX_SIG) {
                    break;
                }
                const char *sidn = cJSON_GetObjectItem(s, "signal_id")->valuestring;
                signal_id_t sid = sid_from_name(sidn);
                if (sid >= SIG_COUNT) {
                    continue;
                }
                prop_sig_t *ps = &out->sig[out->nsig++];
                ps->sid = sid;
                ps->start_bit = (uint16_t)cJSON_GetObjectItem(s, "start_bit")->valuedouble;
                ps->length = (uint8_t)cJSON_GetObjectItem(s, "length")->valuedouble;
                const char *en = cJSON_GetObjectItem(s, "endian")->valuestring;
                ps->intel = (en && strcasecmp(en, "motorola") != 0);
                cJSON *sgn = cJSON_GetObjectItem(s, "signed");
                ps->is_signed = sgn && cJSON_IsTrue(sgn);
                ps->scale = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(s, "scale"));
                if (ps->scale == 0) {
                    ps->scale = 1.f;
                }
                ps->offset = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(s, "offset"));
            }
        }
        s_nmsg++;
    }
    s_ntell = 0;
    cJSON *tells = cJSON_GetObjectItem(root, "telltale_map");
    if (cJSON_IsArray(tells)) {
        cJSON *t;
        cJSON_ArrayForEach(t, tells) {
            if (s_ntell >= PROP_MAX_TELL) {
                break;
            }
            signal_id_t from = sid_from_name(cJSON_GetObjectItem(t, "signal_id")->valuestring);
            const char *icon = cJSON_GetObjectItem(t, "icon")->valuestring;
            signal_id_t ic = sid_from_name(icon);
            if (from >= SIG_COUNT || ic >= SIG_COUNT) {
                continue;
            }
            s_tell[s_ntell].from = from;
            s_tell[s_ntell].icon = ic;
            s_tell[s_ntell].on_value = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(t, "on_value"));
            s_ntell++;
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "loaded %d proprietary messages", s_nmsg);
    return ESP_OK;
}

typedef struct {
    uint32_t id;
    uint32_t id_mask;
    uint32_t mux_val;
    uint16_t mux_start;
    uint8_t ext;
    uint8_t dlc;
    uint8_t has_mux;
    uint8_t mux_len;
    uint8_t nsig;
    uint8_t pad[3];
    struct {
        float scale;
        float offset;
        uint16_t start_bit;
        uint8_t length;
        uint8_t sid;
        uint8_t intel;
        uint8_t is_signed;
        uint8_t pad[2];
    } sig[PROP_MAX_SIG];
} prop_msg_store_t;

typedef struct {
    uint8_t from;
    uint8_t icon;
    uint8_t pad[2];
    float on_value;
} prop_tell_store_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t nmsg;
    uint16_t ntell;
    uint16_t reserved;
    prop_msg_store_t msg[PROP_MAX_MSG];
    prop_tell_store_t tell[PROP_MAX_TELL];
} prop_store_t;

_Static_assert(sizeof(prop_store_t) <= 8192, "proprietary NVS record too large");

#define PROP_NVS_MAGIC 0x50524F31u

static void store_from_live(prop_store_t *st)
{
    memset(st, 0, sizeof(*st));
    st->magic = PROP_NVS_MAGIC;
    st->version = 1;
    st->nmsg = (uint16_t)s_nmsg;
    st->ntell = (uint16_t)s_ntell;
    for (int i = 0; i < s_nmsg; i++) {
        prop_msg_store_t *d = &st->msg[i];
        d->id = s_msg[i].id;
        d->id_mask = s_msg[i].id_mask;
        d->mux_val = s_msg[i].mux_val;
        d->mux_start = s_msg[i].mux_start;
        d->ext = s_msg[i].ext;
        d->dlc = s_msg[i].dlc;
        d->has_mux = s_msg[i].has_mux;
        d->mux_len = s_msg[i].mux_len;
        d->nsig = (uint8_t)s_msg[i].nsig;
        for (int s = 0; s < s_msg[i].nsig && s < PROP_MAX_SIG; s++) {
            d->sig[s].scale = s_msg[i].sig[s].scale;
            d->sig[s].offset = s_msg[i].sig[s].offset;
            d->sig[s].start_bit = s_msg[i].sig[s].start_bit;
            d->sig[s].length = s_msg[i].sig[s].length;
            d->sig[s].sid = (uint8_t)s_msg[i].sig[s].sid;
            d->sig[s].intel = s_msg[i].sig[s].intel;
            d->sig[s].is_signed = s_msg[i].sig[s].is_signed;
        }
    }
    for (int i = 0; i < s_ntell; i++) {
        st->tell[i].from = (uint8_t)s_tell[i].from;
        st->tell[i].icon = (uint8_t)s_tell[i].icon;
        st->tell[i].on_value = s_tell[i].on_value;
    }
}

static void live_from_store(const prop_store_t *st)
{
    memset(s_msg, 0, sizeof(s_msg));
    memset(s_tell, 0, sizeof(s_tell));
    s_nmsg = st->nmsg > PROP_MAX_MSG ? PROP_MAX_MSG : st->nmsg;
    s_ntell = st->ntell > PROP_MAX_TELL ? PROP_MAX_TELL : st->ntell;
    for (int i = 0; i < s_nmsg; i++) {
        const prop_msg_store_t *d = &st->msg[i];
        s_msg[i].id = d->id;
        s_msg[i].id_mask = d->id_mask ? d->id_mask : 0x1FFFFFFF;
        s_msg[i].mux_val = d->mux_val;
        s_msg[i].mux_start = d->mux_start;
        s_msg[i].ext = d->ext;
        s_msg[i].dlc = d->dlc;
        s_msg[i].has_mux = d->has_mux;
        s_msg[i].mux_len = d->mux_len;
        s_msg[i].nsig = d->nsig > PROP_MAX_SIG ? PROP_MAX_SIG : d->nsig;
        for (int s = 0; s < s_msg[i].nsig; s++) {
            if (d->sig[s].sid >= SIG_COUNT || d->sig[s].length == 0) {
                continue;
            }
            prop_sig_t *ps = &s_msg[i].sig[s];
            ps->scale = d->sig[s].scale == 0.f ? 1.f : d->sig[s].scale;
            ps->offset = d->sig[s].offset;
            ps->start_bit = d->sig[s].start_bit;
            ps->length = d->sig[s].length;
            ps->sid = (signal_id_t)d->sig[s].sid;
            ps->intel = d->sig[s].intel;
            ps->is_signed = d->sig[s].is_signed;
        }
    }
    for (int i = 0; i < s_ntell; i++) {
        if (st->tell[i].from >= SIG_COUNT || st->tell[i].icon >= SIG_COUNT) {
            continue;
        }
        s_tell[i].from = (signal_id_t)st->tell[i].from;
        s_tell[i].icon = (signal_id_t)st->tell[i].icon;
        s_tell[i].on_value = st->tell[i].on_value;
    }
}

static void load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("prop", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    prop_store_t *st = calloc(1, sizeof(*st));
    if (!st) {
        nvs_close(h);
        return;
    }
    size_t len = sizeof(*st);
    esp_err_t err = nvs_get_blob(h, "table", st, &len);
    nvs_close(h);
    if (err == ESP_OK && len == sizeof(*st) && st->magic == PROP_NVS_MAGIC && st->version == 1) {
        live_from_store(st);
        ESP_LOGI(TAG, "NVS table loaded (%d msgs)", s_nmsg);
    }
    free(st);
}

void proprietary_init_default(void)
{
    memset(s_msg, 0, sizeof(s_msg));
    memset(s_tell, 0, sizeof(s_tell));
    s_nmsg = 1;
    s_msg[0].id = 0x18FF50E5;
    s_msg[0].id_mask = 0x1FFFFFFF;
    s_msg[0].ext = true;
    s_msg[0].dlc = 8;
    s_msg[0].has_mux = true;
    s_msg[0].mux_start = 0;
    s_msg[0].mux_len = 8;
    s_msg[0].mux_val = 1;
    s_msg[0].nsig = 1;
    s_msg[0].sig[0] = (prop_sig_t){
        .sid = SIG_RPM, .start_bit = 16, .length = 16, .intel = true, .scale = 0.125f, .offset = 0
    };
    s_ntell = 1;
    s_tell[0] = (prop_tell_t){ .from = SIG_MIL, .icon = SIG_MIL, .on_value = 1.f };
    load_nvs();
    ESP_LOGI(TAG, "proprietary table ready (%d msgs)", s_nmsg);
}

esp_err_t proprietary_add(const proprietary_entry_t *e)
{
    if (!e || e->sid >= SIG_COUNT || e->length == 0 || e->length > 32 || e->start_bit > 63) {
        return ESP_ERR_INVALID_ARG;
    }
    if (e->dlc > 8) {
        return ESP_ERR_INVALID_ARG;
    }
    if (e->has_mux && (e->mux_len == 0 || e->mux_len > 32 || e->mux_start > 63)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t mask = e->id_mask ? e->id_mask : 0x1FFFFFFF;
    for (int i = 0; i < s_nmsg; i++) {
        prop_msg_t *m = &s_msg[i];
        bool same = m->id == e->id && m->id_mask == mask && m->ext == e->extended && m->has_mux == e->has_mux;
        if (same && e->has_mux) {
            same = m->mux_start == e->mux_start && m->mux_len == e->mux_len && m->mux_val == e->mux_val;
        }
        if (!same) {
            continue;
        }
        if (m->nsig >= PROP_MAX_SIG) {
            return ESP_ERR_NO_MEM;
        }
        if (e->dlc > m->dlc) {
            m->dlc = e->dlc;
        }
        prop_sig_t *ps = &m->sig[m->nsig++];
        memset(ps, 0, sizeof(*ps));
        ps->sid = e->sid;
        ps->start_bit = e->start_bit;
        ps->length = e->length;
        ps->intel = e->intel;
        ps->is_signed = e->is_signed;
        ps->scale = e->scale == 0.f ? 1.f : e->scale;
        ps->offset = e->offset;
        return ESP_OK;
    }
    if (s_nmsg >= PROP_MAX_MSG) {
        return ESP_ERR_NO_MEM;
    }
    prop_msg_t *m = &s_msg[s_nmsg++];
    memset(m, 0, sizeof(*m));
    m->id = e->id;
    m->id_mask = mask;
    m->ext = e->extended;
    m->dlc = e->dlc;
    m->has_mux = e->has_mux;
    m->mux_start = e->mux_start;
    m->mux_len = e->mux_len;
    m->mux_val = e->mux_val;
    m->nsig = 1;
    m->sig[0].sid = e->sid;
    m->sig[0].start_bit = e->start_bit;
    m->sig[0].length = e->length;
    m->sig[0].intel = e->intel;
    m->sig[0].is_signed = e->is_signed;
    m->sig[0].scale = e->scale == 0.f ? 1.f : e->scale;
    m->sig[0].offset = e->offset;
    return ESP_OK;
}

esp_err_t proprietary_save_nvs(void)
{
    prop_store_t *st = calloc(1, sizeof(*st));
    if (!st) {
        return ESP_ERR_NO_MEM;
    }
    store_from_live(st);
    nvs_handle_t h;
    esp_err_t err = nvs_open("prop", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "table", st, sizeof(*st));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    free(st);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS table saved (%d msgs)", s_nmsg);
    }
    return err;
}

esp_err_t proprietary_export_json(char *buf, size_t cap, size_t *out_len)
{
    if (!buf || cap < 3) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "messages", messages);
    for (int i = 0; i < s_nmsg; i++) {
        prop_msg_t *m = &s_msg[i];
        cJSON *jm = cJSON_CreateObject();
        char idb[16];
        snprintf(idb, sizeof(idb), "0x%08lX", (unsigned long)m->id);
        cJSON_AddStringToObject(jm, "id", idb);
        snprintf(idb, sizeof(idb), "0x%08lX", (unsigned long)m->id_mask);
        cJSON_AddStringToObject(jm, "id_mask", idb);
        cJSON_AddBoolToObject(jm, "extended", m->ext);
        cJSON_AddNumberToObject(jm, "dlc", m->dlc);
        if (m->has_mux) {
            cJSON *mux = cJSON_CreateObject();
            cJSON_AddNumberToObject(mux, "start_bit", m->mux_start);
            cJSON_AddNumberToObject(mux, "length", m->mux_len);
            cJSON_AddNumberToObject(mux, "value", m->mux_val);
            cJSON_AddItemToObject(jm, "mux", mux);
        }
        cJSON *sigs = cJSON_CreateArray();
        for (int s = 0; s < m->nsig; s++) {
            cJSON *js = cJSON_CreateObject();
            cJSON_AddStringToObject(js, "signal_id", signal_name(m->sig[s].sid));
            cJSON_AddNumberToObject(js, "start_bit", m->sig[s].start_bit);
            cJSON_AddNumberToObject(js, "length", m->sig[s].length);
            cJSON_AddStringToObject(js, "endian", m->sig[s].intel ? "intel" : "motorola");
            cJSON_AddBoolToObject(js, "signed", m->sig[s].is_signed);
            cJSON_AddNumberToObject(js, "scale", m->sig[s].scale);
            cJSON_AddNumberToObject(js, "offset", m->sig[s].offset);
            cJSON_AddItemToArray(sigs, js);
        }
        cJSON_AddItemToObject(jm, "signals", sigs);
        cJSON_AddItemToArray(messages, jm);
    }
    cJSON *tells = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "telltale_map", tells);
    for (int i = 0; i < s_ntell; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "signal_id", signal_name(s_tell[i].from));
        cJSON_AddStringToObject(t, "icon", signal_name(s_tell[i].icon));
        cJSON_AddNumberToObject(t, "on_value", s_tell[i].on_value);
        cJSON_AddItemToArray(tells, t);
    }
    char *printed = cJSON_Print(root);
    cJSON_Delete(root);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }
    size_t n = strlen(printed);
    if (n + 1 > cap) {
        free(printed);
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf, printed, n + 1);
    free(printed);
    if (out_len) {
        *out_len = n;
    }
    return ESP_OK;
}
