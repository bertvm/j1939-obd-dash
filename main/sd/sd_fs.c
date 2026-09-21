#include "sd_fs.h"
#include "board.h"
#include "pins.h"
#include "proprietary.h"
#include "dtc.h"
#include "signals.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "sd";
static sdmmc_card_t *s_card;
static sdmmc_host_t s_host;
static bool s_mounted;
static bool s_logging;
static FILE *s_log;
static char s_status[96] = "SD: idle";

#define MOUNT_POINT "/sdcard"

static void apply_spn_json(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    fread(buf, 1, (size_t)n, f);
    buf[n] = 0;
    fclose(f);
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return;
    }
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (cJSON_IsString(item) && item->string) {
            uint32_t spn = (uint32_t)strtoul(item->string, NULL, 10);
            dtc_add_spn_name(spn, item->valuestring);
        }
    }
    cJSON_Delete(root);
}

static void apply_obd_json(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    fread(buf, 1, (size_t)n, f);
    buf[n] = 0;
    fclose(f);
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return;
    }
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (cJSON_IsString(item) && item->string) {
            uint16_t code = (uint16_t)strtoul(item->string, NULL, 0);
            dtc_add_obd_title(code, item->valuestring);
        }
    }
    cJSON_Delete(root);
}

esp_err_t sd_fs_init(void)
{
    board_sd_cs(true);
    s_host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_SD_MOSI,
        .miso_io_num = BOARD_SD_MISO,
        .sclk_io_num = BOARD_SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t ret = spi_bus_initialize(s_host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        snprintf(s_status, sizeof(s_status), "SD: SPI fail");
        return ret;
    }
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = GPIO_NUM_NC;
    slot.host_id = s_host.slot;
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &s_host, &slot, &mount, &s_card);
    if (ret != ESP_OK) {
        snprintf(s_status, sizeof(s_status), "SD: not present");
        ESP_LOGW(TAG, "mount failed %s", esp_err_to_name(ret));
        return ret;
    }
    s_mounted = true;
    mkdir(MOUNT_POINT "/can", 0755);
    mkdir(MOUNT_POINT "/log", 0755);
    snprintf(s_status, sizeof(s_status), "SD: mounted");
    sd_fs_reload_database();
    return ESP_OK;
}

esp_err_t sd_fs_reload_database(void)
{
    if (!s_mounted) {
        snprintf(s_status, sizeof(s_status), "SD: not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    FILE *f = fopen(MOUNT_POINT "/can/proprietary.json", "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc((size_t)n + 1);
        if (buf) {
            fread(buf, 1, (size_t)n, f);
            buf[n] = 0;
            if (proprietary_load_json(buf, (size_t)n) != ESP_OK) {
                snprintf(s_status, sizeof(s_status), "SD: bad proprietary.json");
            } else {
                snprintf(s_status, sizeof(s_status), "SD: DB loaded");
            }
            free(buf);
        }
        fclose(f);
    } else {
        snprintf(s_status, sizeof(s_status), "SD: no proprietary.json (using built-in)");
    }
    apply_spn_json(MOUNT_POINT "/can/spn_names.json");
    apply_obd_json(MOUNT_POINT "/can/obd_codes.json");
    return ESP_OK;
}

esp_err_t sd_fs_save_proprietary(const char *json)
{
    if (!s_mounted || !json) {
        return ESP_ERR_INVALID_STATE;
    }
    mkdir(MOUNT_POINT "/can", 0755);
    FILE *f = fopen(MOUNT_POINT "/can/proprietary.json", "w");
    if (!f) {
        snprintf(s_status, sizeof(s_status), "SD: cannot write proprietary.json");
        return ESP_FAIL;
    }
    size_t n = strlen(json);
    size_t w = fwrite(json, 1, n, f);
    fclose(f);
    if (w != n) {
        snprintf(s_status, sizeof(s_status), "SD: proprietary.json write failed");
        return ESP_FAIL;
    }
    snprintf(s_status, sizeof(s_status), "SD: proprietary.json saved");
    return ESP_OK;
}

void sd_fs_toggle_log(void)
{
    if (!s_mounted) {
        return;
    }
    if (s_logging) {
        if (s_log) {
            fflush(s_log);
            fclose(s_log);
            s_log = NULL;
        }
        s_logging = false;
        snprintf(s_status, sizeof(s_status), "SD: log stopped");
        return;
    }
    s_log = fopen(MOUNT_POINT "/log/signals.csv", "a");
    if (!s_log) {
        snprintf(s_status, sizeof(s_status), "SD: log open fail");
        return;
    }
    fprintf(s_log, "t_us,name,value,unit\n");
    s_logging = true;
    snprintf(s_status, sizeof(s_status), "SD: logging");
}

void sd_fs_log_signals(void)
{
    if (!s_logging || !s_log) {
        return;
    }
    int64_t t = esp_timer_get_time();
    for (int i = 0; i < SIG_HOURS; i++) {
        float v;
        signal_quality_t q;
        if (!signals_get((signal_id_t)i, &v, &q, NULL) || q == SIG_Q_STALE) {
            continue;
        }
        fprintf(s_log, "%lld,%s,%.3f,%s\n", (long long)t, signal_name((signal_id_t)i), v, signal_unit((signal_id_t)i));
    }
}

void sd_fs_log_dtc_line(const char *line)
{
    if (!s_logging || !s_log || !line) {
        return;
    }
    fprintf(s_log, "%lld,dtc,%s,\n", (long long)esp_timer_get_time(), line);
    fflush(s_log);
}

void sd_fs_log_cmd(const char *line)
{
    if (!s_logging || !s_log || !line) {
        return;
    }
    fprintf(s_log, "%lld,cmd,%s,\n", (long long)esp_timer_get_time(), line);
    fflush(s_log);
}

const char *sd_fs_status(void)
{
    return s_status;
}

bool sd_fs_mounted(void)
{
    return s_mounted;
}

bool sd_fs_logging(void)
{
    return s_logging;
}
