#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_HOST SPI3_HOST
#define LCD_WIDTH 240
#define LCD_HEIGHT 240

#define PIN_LCD_MOSI 41
#define PIN_LCD_SCLK 42
#define PIN_LCD_DC 40
#define PIN_LCD_CS 21
#define PIN_LCD_RST 45
#define PIN_LCD_BL 20

#define PIN_POWER_HOLD 2
#define PIN_BUTTON_MAIN 0
#define PIN_BUTTON_UP 10
#define PIN_BUTTON_DOWN 39

#define RGB565(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

static const char *TAG = "manmanshuo";
static esp_lcd_panel_handle_t panel;
static uint16_t *framebuffer;

typedef struct {
    char c;
    uint8_t rows[7];
} glyph_t;

static const glyph_t font[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {' ', {0, 0, 0, 0, 0, 0, 0}},
};

static const uint8_t *find_glyph(char c)
{
    for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); ++i) {
        if (font[i].c == c) return font[i].rows;
    }
    return font[sizeof(font) / sizeof(font[0]) - 1].rows;
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            framebuffer[(y + row) * LCD_WIDTH + x + col] = color;
        }
    }
}

static void flush_screen(void)
{
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, framebuffer));
}

static void draw_char(int x, int y, char c, int scale, uint16_t color)
{
    const uint8_t *rows = find_glyph(c);
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (rows[row] & (1U << (4 - col))) {
                fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(int x, int y, const char *text, int scale, uint16_t color)
{
    while (*text) {
        draw_char(x, y, *text++, scale, color);
        x += 6 * scale;
    }
}

static void draw_text_centered(int y, const char *text, int scale, uint16_t color)
{
    const int width = (int)strlen(text) * 6 * scale - scale;
    draw_text((LCD_WIDTH - width) / 2, y, text, scale, color);
}

static void draw_button(int x, bool pressed, uint16_t active, const char *label)
{
    fill_rect(x, 188, 80, 52, pressed ? active : RGB565(42, 49, 68));
    draw_text(x + (80 - ((int)strlen(label) * 12 - 2)) / 2,
              205, label, 2, RGB565(255, 255, 255));
}

static void init_power_and_buttons(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_POWER_HOLD) | (1ULL << PIN_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    ESP_ERROR_CHECK(gpio_set_level(PIN_POWER_HOLD, 1));
    ESP_ERROR_CHECK(gpio_set_level(PIN_LCD_BL, 0));

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_BUTTON_MAIN) | (1ULL << PIN_BUTTON_UP) | (1ULL << PIN_BUTTON_DOWN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));
}

static void init_display(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 3,
        .pclk_hz = 80 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
}

void app_main(void)
{
    ESP_LOGI(TAG, "stage 1: power and buttons");
    init_power_and_buttons();
    ESP_LOGI(TAG, "stage 2: display init");
    init_display();
    vTaskDelay(pdMS_TO_TICKS(50));

    framebuffer = heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_ERROR_CHECK(framebuffer ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "stage 3: framebuffer ready (%u bytes)",
             LCD_WIDTH * LCD_HEIGHT * (unsigned)sizeof(uint16_t));

    fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, RGB565(246, 243, 235));
    fill_rect(0, 0, LCD_WIDTH, 8, RGB565(245, 151, 79));
    draw_text_centered(42, "MANMANSHUO", 3, RGB565(37, 42, 58));
    draw_text_centered(105, "HARDWARE", 2, RGB565(91, 98, 118));
    draw_text_centered(137, "READY", 3, RGB565(39, 145, 108));
    draw_button(0, false, RGB565(245, 151, 79), "MAIN");
    draw_button(80, false, RGB565(39, 145, 108), "UP");
    draw_button(160, false, RGB565(74, 120, 220), "DOWN");
    ESP_LOGI(TAG, "stage 4: sending first frame");
    flush_screen();
    ESP_ERROR_CHECK(gpio_set_level(PIN_LCD_BL, 1));
    ESP_LOGI(TAG, "stage 5: screen and backlight enabled");

    bool old_main = false, old_up = false, old_down = false;
    const uint16_t test_colors[] = {
        RGB565(255, 0, 0),
        RGB565(0, 255, 0),
        RGB565(0, 0, 255),
        RGB565(255, 255, 255),
    };
    size_t color_index = 0;
    TickType_t last_color_change = xTaskGetTickCount();
    while (true) {
        bool main_pressed = gpio_get_level(PIN_BUTTON_MAIN) == 0;
        bool up_pressed = gpio_get_level(PIN_BUTTON_UP) == 0;
        bool down_pressed = gpio_get_level(PIN_BUTTON_DOWN) == 0;
        if (main_pressed != old_main) draw_button(0, main_pressed, RGB565(245, 151, 79), "MAIN");
        if (up_pressed != old_up) draw_button(80, up_pressed, RGB565(39, 145, 108), "UP");
        if (down_pressed != old_down) draw_button(160, down_pressed, RGB565(74, 120, 220), "DOWN");
        if (main_pressed != old_main || up_pressed != old_up || down_pressed != old_down) {
            flush_screen();
            ESP_LOGI(TAG, "buttons main=%d up=%d down=%d", main_pressed, up_pressed, down_pressed);
        }
        old_main = main_pressed;
        old_up = up_pressed;
        old_down = down_pressed;

        if (xTaskGetTickCount() - last_color_change >= pdMS_TO_TICKS(1000)) {
            fill_rect(0, 0, LCD_WIDTH, 188, test_colors[color_index]);
            flush_screen();
            ESP_LOGI(TAG, "display test color %u", (unsigned)color_index + 1);
            color_index = (color_index + 1) % (sizeof(test_colors) / sizeof(test_colors[0]));
            last_color_change = xTaskGetTickCount();
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}
