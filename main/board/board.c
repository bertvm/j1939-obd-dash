#include "board.h"
#include "pins.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "hal/lcd_types.h"

static const char *TAG = "board";
static uint8_t s_ch422g;
static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_ch422_mode;
static i2c_master_dev_handle_t s_ch422_oc;

static esp_err_t ch422g_flush(void)
{
    return i2c_master_transmit(s_ch422_oc, &s_ch422g, 1, 1000);
}

esp_err_t board_i2c_init(void)
{
    if (s_i2c) {
        return ESP_OK;
    }
    i2c_master_bus_config_t bus = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_i2c), TAG, "i2c bus");
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    dev.device_address = CH422G_ADDR_MODE;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev, &s_ch422_mode), TAG, "ch422 mode");
    dev.device_address = CH422G_ADDR_OC;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev, &s_ch422_oc), TAG, "ch422 oc");
    return ESP_OK;
}

esp_err_t board_ch422g_set_bit(int bit, int level)
{
    if (level) {
        s_ch422g |= (uint8_t)(1u << bit);
    } else {
        s_ch422g &= (uint8_t)~(1u << bit);
    }
    return ch422g_flush();
}

uint8_t board_ch422g_shadow(void)
{
    return s_ch422g;
}

static esp_err_t ch422g_output_mode(void)
{
    uint8_t mode = 0x01;
    return i2c_master_transmit(s_ch422_mode, &mode, 1, 1000);
}

esp_err_t board_set_can_mode(bool enable_can)
{
    return board_ch422g_set_bit(CH422G_BIT_CAN_SEL, enable_can ? 1 : 0);
}

esp_err_t board_backlight_on(void)
{
    return board_ch422g_set_bit(CH422G_BIT_BL, 1);
}

esp_err_t board_sd_cs(bool select)
{
    /* EXIO4 low selects the card (Waveshare SD example). */
    return board_ch422g_set_bit(CH422G_BIT_SD_CS, select ? 0 : 1);
}

static esp_err_t touch_reset(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_INT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio4");
    ESP_RETURN_ON_ERROR(ch422g_output_mode(), TAG, "ch422 mode");

    s_ch422g = 0x2C;
    ESP_RETURN_ON_ERROR(ch422g_flush(), TAG, "touch rst 1");
    esp_rom_delay_us(100 * 1000);
    gpio_set_level(BOARD_TOUCH_INT_GPIO, 0);
    esp_rom_delay_us(100 * 1000);
    s_ch422g = 0x2E;
    ESP_RETURN_ON_ERROR(ch422g_flush(), TAG, "touch rst 2");
    esp_rom_delay_us(200 * 1000);
    return ESP_OK;
}

esp_err_t board_init(esp_lcd_panel_handle_t *panel, esp_lcd_touch_handle_t *touch)
{
    if (!panel || !touch) {
        return ESP_ERR_INVALID_ARG;
    }
    *panel = NULL;
    *touch = NULL;

    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(ch422g_output_mode(), TAG, "ch422");
    s_ch422g = (1u << CH422G_BIT_LCD_RST) | (1u << CH422G_BIT_TP_RST) | (1u << CH422G_BIT_CAN_SEL);
    ESP_RETURN_ON_ERROR(ch422g_flush(), TAG, "expander");

    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = BOARD_LCD_PCLK_HZ,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags = { .pclk_active_neg = 1 },
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .bounce_buffer_size_px = BOARD_LCD_H_RES * BOARD_RGB_BOUNCE_LINES,
        .dma_burst_size = 64,
        .disp_gpio_num = GPIO_NUM_NC,
        .flags = { .fb_in_psram = 1 },
    };
    panel_config.hsync_gpio_num = BOARD_LCD_HSYNC;
    panel_config.vsync_gpio_num = BOARD_LCD_VSYNC;
    panel_config.de_gpio_num = BOARD_LCD_DE;
    panel_config.pclk_gpio_num = BOARD_LCD_PCLK;
    const gpio_num_t data_pins[16] = {
        BOARD_LCD_DATA0, BOARD_LCD_DATA1, BOARD_LCD_DATA2, BOARD_LCD_DATA3,
        BOARD_LCD_DATA4, BOARD_LCD_DATA5, BOARD_LCD_DATA6, BOARD_LCD_DATA7,
        BOARD_LCD_DATA8, BOARD_LCD_DATA9, BOARD_LCD_DATA10, BOARD_LCD_DATA11,
        BOARD_LCD_DATA12, BOARD_LCD_DATA13, BOARD_LCD_DATA14, BOARD_LCD_DATA15,
    };
    for (int i = 0; i < 16; i++) {
        panel_config.data_gpio_nums[i] = data_pins[i];
    }

    ESP_LOGI(TAG, "RGB LCD init 800x480 bounce=%d", BOARD_RGB_BOUNCE_LINES);
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&panel_config, panel), TAG, "rgb panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), TAG, "panel init");

    ESP_RETURN_ON_ERROR(touch_reset(), TAG, "touch reset");
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_config.scl_speed_hz = BOARD_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_config, &tp_io), TAG, "tp io");
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, touch), TAG, "gt911");

    ESP_RETURN_ON_ERROR(board_backlight_on(), TAG, "bl");
    ESP_RETURN_ON_ERROR(board_set_can_mode(true), TAG, "can_sel");
    ESP_LOGI(TAG, "CAN_SEL high (USB OTG pins are CAN TX/RX)");
    return ESP_OK;
}
