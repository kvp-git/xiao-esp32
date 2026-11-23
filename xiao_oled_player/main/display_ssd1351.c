#include "display_ssd1351.h"
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static spi_device_handle_t spi_dev;

static void lcd_cmd(uint8_t cmd) {
    gpio_set_level(PIN_NUM_DC, 0);
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    spi_device_polling_transmit(spi_dev, &t);
}

static void lcd_data(const uint8_t *data, size_t len) {
    if (len == 0) return;
    gpio_set_level(PIN_NUM_DC, 1);
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(spi_dev, &t);
}

static void lcd_data_byte(uint8_t val) {
    lcd_data(&val, 1);
}

esp_err_t ssd1351_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_NUM_DC) | (1ULL << PIN_NUM_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);

    // Hardware reset
    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = OLED_WIDTH * OLED_HEIGHT * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 20 * 1000 * 1000, // 20 MHz SPI
        .mode = 0,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 2,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spi_dev));

    // SSD1351 Initialization Sequence
    lcd_cmd(0xFD); lcd_data_byte(0x12); // Unlock OLED driver IC
    lcd_cmd(0xFD); lcd_data_byte(0xB1); // Unlock all commands
    lcd_cmd(0xAE);                      // Display OFF
    lcd_cmd(0xB3); lcd_data_byte(0xF1); // Front clock divider / osc freq
    lcd_cmd(0xCA); lcd_data_byte(0x7F); // Multiplex ratio = 128
    lcd_cmd(0xA0); lcd_data_byte(0x74); // Color depth 65K, RGB mode, scan direction
    lcd_cmd(0x15); uint8_t col[] = {0x00, 0x7F}; lcd_data(col, 2);
    lcd_cmd(0x75); uint8_t row[] = {0x00, 0x7F}; lcd_data(row, 2);
    lcd_cmd(0xA1); lcd_data_byte(0x00); // Display start line = 0
    lcd_cmd(0xA2); lcd_data_byte(0x00); // Display offset = 0
    lcd_cmd(0xB5); lcd_data_byte(0x00); // Set GPIO
    lcd_cmd(0xAB); lcd_data_byte(0x01); // Enable internal VDD regulator
    lcd_cmd(0xB1); lcd_data_byte(0x32); // Phase 1 & 2 period
    lcd_cmd(0xBE); lcd_data_byte(0x05); // VCOMH voltage
    lcd_cmd(0xA6);                      // Normal display mode
    lcd_cmd(0xC1); uint8_t contrast[] = {0xC8, 0x80, 0xC8}; lcd_data(contrast, 3);
    lcd_cmd(0xC7); lcd_data_byte(0x0F); // Master contrast current
    lcd_cmd(0xB4); uint8_t vsl[] = {0xA0, 0xB5, 0x55}; lcd_data(vsl, 3);
    lcd_cmd(0xB6); lcd_data_byte(0x01); // Pre-charge 2nd period
    lcd_cmd(0xAF);                      // Display ON

    ssd1351_clear();
    return ESP_OK;
}

void ssd1351_draw_frame(const uint16_t *rgb565_be_buf) {
    uint8_t col[] = {0x00, 0x7F};
    uint8_t row[] = {0x00, 0x7F};
    lcd_cmd(0x15); lcd_data(col, 2);
    lcd_cmd(0x75); lcd_data(row, 2);
    lcd_cmd(0x5C); // Write RAM

    gpio_set_level(PIN_NUM_DC, 1);
    spi_transaction_t t = {
        .length = OLED_WIDTH * OLED_HEIGHT * 16,
        .tx_buffer = rgb565_be_buf,
    };
    spi_device_transmit(spi_dev, &t);
}

void ssd1351_clear(void) {
    uint16_t *blank = heap_caps_calloc(OLED_WIDTH * OLED_HEIGHT, sizeof(uint16_t), MALLOC_CAP_DMA);
    if (blank) {
        ssd1351_draw_frame(blank);
        free(blank);
    }
}
