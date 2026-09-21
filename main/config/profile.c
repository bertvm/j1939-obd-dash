#include "profile.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "profile";
static dash_profile_t s_p;

void profile_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    s_p.profile = PROFILE_J1939_250;
    s_p.j1939_tx_enable = false;
    s_p.obd_requests = false;
    s_p.metric = true;
    s_p.our_sa = 0xF9;
    s_p.engine_sa = 0x00;
    s_p.tsc1_rpm_cap = 1200;
    s_p.brightness = 80;
    s_p.prop_bitrate = 250000;
    s_p.prop_extended = true;

    nvs_handle_t h;
    if (nvs_open("dash", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    if (nvs_get_u8(h, "profile", &u8) == ESP_OK && u8 < PROFILE_COUNT) {
        s_p.profile = u8;
    }
    if (nvs_get_u8(h, "j1939tx", &u8) == ESP_OK) {
        s_p.j1939_tx_enable = u8;
    }
    if (nvs_get_u8(h, "obdreq", &u8) == ESP_OK) {
        s_p.obd_requests = u8;
    }
    if (nvs_get_u8(h, "metric", &u8) == ESP_OK) {
        s_p.metric = u8;
    }
    if (nvs_get_u8(h, "our_sa", &u8) == ESP_OK) {
        s_p.our_sa = u8;
    }
    if (nvs_get_u8(h, "eng_sa", &u8) == ESP_OK) {
        s_p.engine_sa = u8;
    }
    if (nvs_get_u16(h, "rpmcap", &u16) == ESP_OK && u16 >= 600 && u16 <= 4000) {
        s_p.tsc1_rpm_cap = u16;
    }
    if (nvs_get_u8(h, "bright", &u8) == ESP_OK) {
        s_p.brightness = u8;
    }
    if (nvs_get_u32(h, "prop_bps", &u32) == ESP_OK) {
        s_p.prop_bitrate = u32;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded profile=%d tx=%d", (int)s_p.profile, s_p.j1939_tx_enable);
}

dash_profile_t *profile_get(void)
{
    return &s_p;
}

void profile_save(void)
{
    nvs_handle_t h;
    if (nvs_open("dash", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, "profile", (uint8_t)s_p.profile);
    nvs_set_u8(h, "j1939tx", s_p.j1939_tx_enable);
    nvs_set_u8(h, "obdreq", s_p.obd_requests);
    nvs_set_u8(h, "metric", s_p.metric);
    nvs_set_u8(h, "our_sa", s_p.our_sa);
    nvs_set_u8(h, "eng_sa", s_p.engine_sa);
    nvs_set_u16(h, "rpmcap", s_p.tsc1_rpm_cap);
    nvs_set_u8(h, "bright", s_p.brightness);
    nvs_set_u32(h, "prop_bps", s_p.prop_bitrate);
    nvs_commit(h);
    nvs_close(h);
}

uint32_t profile_bitrate(void)
{
    switch (s_p.profile) {
    case PROFILE_J1939_500:
    case PROFILE_OBD_500:
        return 500000;
    case PROFILE_PROP_CUSTOM:
        return s_p.prop_bitrate ? s_p.prop_bitrate : 250000;
    default:
        return 250000;
    }
}

bool profile_use_j1939(void)
{
    return s_p.profile == PROFILE_J1939_250 || s_p.profile == PROFILE_J1939_500 || s_p.profile == PROFILE_MIXED_250;
}

bool profile_use_obd(void)
{
    return s_p.profile == PROFILE_OBD_500;
}

bool profile_use_prop(void)
{
    return s_p.profile == PROFILE_MIXED_250 || s_p.profile == PROFILE_PROP_CUSTOM;
}

bool profile_listen_only(void)
{
    if (profile_use_obd()) {
        return !s_p.obd_requests;
    }
    if (profile_use_j1939()) {
        return !s_p.j1939_tx_enable;
    }
    return true;
}

const char *profile_name(vehicle_profile_t p)
{
    switch (p) {
    case PROFILE_J1939_250:
        return "J1939 250k";
    case PROFILE_J1939_500:
        return "J1939 500k";
    case PROFILE_OBD_500:
        return "OBD-II 500k";
    case PROFILE_MIXED_250:
        return "J1939+OEM 250k";
    case PROFILE_PROP_CUSTOM:
        return "Proprietary";
    default:
        return "?";
    }
}
