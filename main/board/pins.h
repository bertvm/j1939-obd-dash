#pragma once

#include "driver/gpio.h"

#define BOARD_LCD_H_RES                 800
#define BOARD_LCD_V_RES                 480
#define BOARD_LCD_PCLK_HZ               (16 * 1000 * 1000)
#define BOARD_RGB_BOUNCE_LINES          10

#define BOARD_LCD_VSYNC                 GPIO_NUM_3
#define BOARD_LCD_HSYNC                 GPIO_NUM_46
#define BOARD_LCD_DE                    GPIO_NUM_5
#define BOARD_LCD_PCLK                  GPIO_NUM_7
#define BOARD_LCD_DATA0                 GPIO_NUM_14
#define BOARD_LCD_DATA1                 GPIO_NUM_38
#define BOARD_LCD_DATA2                 GPIO_NUM_18
#define BOARD_LCD_DATA3                 GPIO_NUM_17
#define BOARD_LCD_DATA4                 GPIO_NUM_10
#define BOARD_LCD_DATA5                 GPIO_NUM_39
#define BOARD_LCD_DATA6                 GPIO_NUM_0
#define BOARD_LCD_DATA7                 GPIO_NUM_45
#define BOARD_LCD_DATA8                 GPIO_NUM_48
#define BOARD_LCD_DATA9                 GPIO_NUM_47
#define BOARD_LCD_DATA10                GPIO_NUM_21
#define BOARD_LCD_DATA11                GPIO_NUM_1
#define BOARD_LCD_DATA12                GPIO_NUM_2
#define BOARD_LCD_DATA13                GPIO_NUM_42
#define BOARD_LCD_DATA14                GPIO_NUM_41
#define BOARD_LCD_DATA15                GPIO_NUM_40

#define BOARD_I2C_PORT                  0
#define BOARD_I2C_SDA                   GPIO_NUM_8
#define BOARD_I2C_SCL                   GPIO_NUM_9
#define BOARD_I2C_FREQ_HZ               400000
#define BOARD_TOUCH_INT_GPIO            GPIO_NUM_4

#define BOARD_TWAI_TX                   GPIO_NUM_20
#define BOARD_TWAI_RX                   GPIO_NUM_19

#define BOARD_SD_MOSI                   GPIO_NUM_11
#define BOARD_SD_MISO                   GPIO_NUM_13
#define BOARD_SD_CLK                    GPIO_NUM_12

#define CH422G_ADDR_MODE                0x24
#define CH422G_ADDR_OC                  0x38
#define CH422G_BIT_LCD_RST              0
#define CH422G_BIT_TP_RST               1
#define CH422G_BIT_BL                   2
#define CH422G_BIT_SD_CS                4
#define CH422G_BIT_CAN_SEL              5
