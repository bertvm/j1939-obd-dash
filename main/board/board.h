#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

esp_err_t board_init(esp_lcd_panel_handle_t *panel, esp_lcd_touch_handle_t *touch);
esp_err_t board_i2c_init(void);
esp_err_t board_ch422g_set_bit(int bit, int level);
uint8_t board_ch422g_shadow(void);
esp_err_t board_set_can_mode(bool enable_can);
esp_err_t board_backlight_on(void);
esp_err_t board_sd_cs(bool select);
