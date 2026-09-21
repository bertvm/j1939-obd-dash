#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t sd_fs_init(void);
esp_err_t sd_fs_reload_database(void);
void sd_fs_toggle_log(void);
void sd_fs_log_signals(void);
void sd_fs_log_dtc_line(const char *line);
void sd_fs_log_cmd(const char *line);
const char *sd_fs_status(void);
esp_err_t sd_fs_save_proprietary(const char *json);
bool sd_fs_mounted(void);
bool sd_fs_logging(void);
