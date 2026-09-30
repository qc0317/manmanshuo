#include <stdbool.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
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

#define PIN_MIC_WS 4
#define PIN_MIC_SCLK 5
#define PIN_MIC_DIN 6

#define AUDIO_SAMPLE_RATE 16000
#define MAX_RECORDING_SECONDS 30
#define MAX_RECORDING_SAMPLES (AUDIO_SAMPLE_RATE * MAX_RECORDING_SECONDS)

#define RGB565(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

static const char *TAG = "manmanshuo";
static esp_lcd_panel_handle_t panel;
static i2s_chan_handle_t mic_rx;
static uint32_t mic_diag_average;
static uint32_t mic_diag_floor;
static uint32_t mic_diag_limit;
static uint32_t mic_diag_signal;
static unsigned mic_diag_warmup;
static unsigned mic_diag_calibration;
static uint16_t *framebuffer;
static int16_t *recording_buffer;
static size_t recording_samples;
static bool recording_full;

typedef struct {
    char c;
    uint8_t rows[7];
} glyph_t;

static const glyph_t font[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'C', {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
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

static void init_microphone(void)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&channel_config, NULL, &mic_rx));

    i2s_std_config_t mic_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_SCLK,
            .ws = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_MIC_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    mic_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mic_rx, &mic_config));
    ESP_ERROR_CHECK(i2s_channel_enable(mic_rx));
}

static unsigned microphone_level(bool capture_audio)
{
    int32_t samples[256];
    static unsigned warmup_count = 0;
    static uint64_t calibration_total = 0;
    static unsigned calibration_count = 0;
    static uint32_t noise_floor = 0;
    static uint32_t smoothed_signal = 0;
    static uint32_t recent_average[5] = {0};
    static unsigned recent_index = 0;
    static unsigned recent_count = 0;
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(mic_rx, samples, sizeof(samples),
                                     &bytes_read, pdMS_TO_TICKS(30));
    if (err != ESP_OK || bytes_read == 0) return 0;

    uint64_t total = 0;
    size_t count = bytes_read / sizeof(samples[0]);
    for (size_t i = 0; i < count; ++i) {
        int64_t value = samples[i] >> 12;
        if (value < 0) value = -value;
        total += (uint32_t)value;

        if (capture_audio && recording_samples < MAX_RECORDING_SAMPLES) {
            int32_t pcm = samples[i] >> 14;
            if (pcm > INT16_MAX) pcm = INT16_MAX;
            if (pcm < INT16_MIN) pcm = INT16_MIN;
            recording_buffer[recording_samples++] = (int16_t)pcm;
        } else if (capture_audio && !recording_full) {
            recording_full = true;
            ESP_LOGW(TAG, "recording reached %d second limit", MAX_RECORDING_SECONDS);
        }
    }

    uint32_t average = count ? (uint32_t)(total / count) : 0;
    mic_diag_average = average;
    if (warmup_count < 64) {
        ++warmup_count;
        mic_diag_warmup = warmup_count;
        return 0;
    }
    if (calibration_count < 128) {
        calibration_total += average;
        ++calibration_count;
        noise_floor = (uint32_t)(calibration_total / calibration_count);
        mic_diag_floor = noise_floor;
        mic_diag_calibration = calibration_count;
        return 0;
    }

    recent_average[recent_index] = average;
    recent_index = (recent_index + 1) % 5;
    if (recent_count < 5) ++recent_count;

    uint32_t ordered[5];
    memcpy(ordered, recent_average, sizeof(ordered));
    for (unsigned i = 1; i < recent_count; ++i) {
        uint32_t value = ordered[i];
        unsigned j = i;
        while (j > 0 && ordered[j - 1] > value) {
            ordered[j] = ordered[j - 1];
            --j;
        }
        ordered[j] = value;
    }
    uint32_t stable_average = ordered[recent_count / 2];

    // Freeze the startup calibration for this hardware test. The previous
    // downward-only adaptation let occasional quiet blocks drag the floor
    // from about 4600 to about 1000, making normal room noise read as 100%.
    uint32_t quiet_limit = noise_floor * 2;
    uint32_t signal = stable_average > quiet_limit ? stable_average - quiet_limit : 0;
    mic_diag_floor = noise_floor;
    mic_diag_limit = quiet_limit;
    mic_diag_signal = signal;
    if (signal > smoothed_signal) {
        smoothed_signal = (smoothed_signal + signal) / 2;
    } else {
        smoothed_signal = smoothed_signal * 3 / 4;
    }

    uint32_t step = noise_floor / 3;
    if (step < 100) step = 100;
    unsigned level = smoothed_signal / step;
    if (level > 11) level = 11;
    return level;
}

static void draw_microphone_meter(unsigned level)
{
    const int meter_x = 10;
    const int meter_y = 112;
    const int meter_w = 220;
    const int meter_h = 34;
    fill_rect(meter_x, meter_y, meter_w, meter_h, RGB565(214, 218, 226));
    if (level > 0) {
        int width = (int)level * 20;
        uint16_t color = level >= 9 ? RGB565(245, 151, 79) : RGB565(39, 145, 108);
        fill_rect(meter_x, meter_y, width, meter_h, color);
    }
}

static void draw_voice_state(const char *title, const char *subtitle,
                             uint16_t accent, bool show_meter)
{
    fill_rect(0, 0, LCD_WIDTH, 188, RGB565(246, 243, 235));
    fill_rect(0, 0, LCD_WIDTH, 8, accent);
    draw_text_centered(32, title, 3, RGB565(37, 42, 58));
    draw_text_centered(76, subtitle, 2, RGB565(91, 98, 118));
    if (show_meter) {
        draw_microphone_meter(0);
    } else {
        fill_rect(42, 116, 156, 6, accent);
    }
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
    recording_buffer = heap_caps_malloc(MAX_RECORDING_SAMPLES * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(recording_buffer ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "stage 3: framebuffer ready (%u bytes)",
             LCD_WIDTH * LCD_HEIGHT * (unsigned)sizeof(uint16_t));
    ESP_LOGI(TAG, "audio buffer ready (%u bytes, %d seconds)",
             (unsigned)(MAX_RECORDING_SAMPLES * sizeof(int16_t)),
             MAX_RECORDING_SECONDS);

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

    ESP_LOGI(TAG, "stage 6: microphone init");
    init_microphone();
    draw_voice_state("HOLD MAIN", "TO SPEAK", RGB565(74, 120, 220), false);
    flush_screen();

    bool old_main = false, old_up = false, old_down = false;
    bool virtual_main = false;
    bool recording = false;
    unsigned old_level = UINT32_MAX;
    unsigned log_counter = 0;
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    ESP_LOGI(TAG, "virtual controls ready: S=start E=end");
    printf("MM:STATE IDLE\n");
    fflush(stdout);
    while (true) {
        int command;
        while ((command = getchar()) != EOF) {
            if (command == 'S' || command == 's') {
                virtual_main = true;
                ESP_LOGI(TAG, "virtual event: start");
            } else if (command == 'E' || command == 'e') {
                virtual_main = false;
                ESP_LOGI(TAG, "virtual event: end");
            }
        }
        clearerr(stdin);

        bool physical_main = gpio_get_level(PIN_BUTTON_MAIN) == 0;
        bool main_pressed = physical_main || virtual_main;
        bool up_pressed = gpio_get_level(PIN_BUTTON_UP) == 0;
        bool down_pressed = gpio_get_level(PIN_BUTTON_DOWN) == 0;
        bool main_changed = main_pressed != old_main;
        if (main_changed) {
            draw_button(0, main_pressed, RGB565(245, 151, 79), "MAIN");
            if (main_pressed) {
                recording = true;
                recording_samples = 0;
                recording_full = false;
                old_level = UINT32_MAX;
                ESP_LOGI(TAG, "recording started");
                printf("MM:STATE LISTENING\n");
                fflush(stdout);
                draw_voice_state("LISTEN", "SPEAK", RGB565(39, 145, 108), true);
                draw_button(0, true, RGB565(245, 151, 79), "MAIN");
                draw_button(80, up_pressed, RGB565(39, 145, 108), "UP");
                draw_button(160, down_pressed, RGB565(74, 120, 220), "DOWN");
            } else if (recording) {
                recording = false;
                uint32_t duration_ms = (uint32_t)(recording_samples * 1000 / AUDIO_SAMPLE_RATE);
                ESP_LOGI(TAG, "recording captured samples=%u duration_ms=%" PRIu32 " full=%d",
                         (unsigned)recording_samples, duration_ms, recording_full);
                printf("MM:CAPTURE samples=%u duration_ms=%" PRIu32 " full=%d\n",
                       (unsigned)recording_samples, duration_ms, recording_full);
                printf("MM:STATE PROCESSING\n");
                fflush(stdout);
                draw_voice_state("CAPTURED", "PROCESS", RGB565(245, 151, 79), false);
                draw_button(0, false, RGB565(245, 151, 79), "MAIN");
                draw_button(80, up_pressed, RGB565(39, 145, 108), "UP");
                draw_button(160, down_pressed, RGB565(74, 120, 220), "DOWN");
                flush_screen();
                vTaskDelay(pdMS_TO_TICKS(1200));
                draw_voice_state("HOLD MAIN", "TO SPEAK", RGB565(74, 120, 220), false);
                draw_button(0, false, RGB565(245, 151, 79), "MAIN");
                draw_button(80, up_pressed, RGB565(39, 145, 108), "UP");
                draw_button(160, down_pressed, RGB565(74, 120, 220), "DOWN");
                printf("MM:STATE IDLE\n");
                fflush(stdout);
            }
        }
        if (up_pressed != old_up) draw_button(80, up_pressed, RGB565(39, 145, 108), "UP");
        if (down_pressed != old_down) draw_button(160, down_pressed, RGB565(74, 120, 220), "DOWN");
        bool buttons_changed = main_pressed != old_main || up_pressed != old_up || down_pressed != old_down;
        if (buttons_changed) {
            ESP_LOGI(TAG, "input voice=%d physical=%d virtual=%d up=%d down=%d",
                     main_pressed, physical_main, virtual_main, up_pressed, down_pressed);
        }
        old_main = main_pressed;
        old_up = up_pressed;
        old_down = down_pressed;

        unsigned level = microphone_level(recording);
        bool level_changed = recording && level != old_level;
        if (level_changed) {
            draw_microphone_meter(level);
            old_level = level;
        }
        if (level_changed || buttons_changed) {
            flush_screen();
        }
        if (++log_counter >= 20) {
            ESP_LOGI(TAG,
                     "mic level=%u/11 avg=%" PRIu32 " floor=%" PRIu32
                     " limit=%" PRIu32 " signal=%" PRIu32 " warmup=%u cal=%u",
                     level, mic_diag_average, mic_diag_floor, mic_diag_limit,
                     mic_diag_signal, mic_diag_warmup, mic_diag_calibration);
            log_counter = 0;
        }
    }
}
