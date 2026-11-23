#pragma once
#include <stdint.h>
#include "esp_err.h"

#define OLED_WIDTH  128
#define OLED_HEIGHT 128

// XIAO ESP32C3 Pinout
#define PIN_NUM_MOSI 10  // D10
#define PIN_NUM_CLK  8   // D8
#define PIN_NUM_CS   3   // D1
#define PIN_NUM_DC   4   // D2
#define PIN_NUM_RST  5   // D3

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ssd1351_init(void);
void ssd1351_draw_frame(const uint16_t *rgb565_be_buf);
void ssd1351_clear(void);

#ifdef __cplusplus
}
#endif
