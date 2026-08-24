/*
 * LED output on GPIO 8.
 *
 * This is the ESP32-C6-DevKitC-1's onboard LED, which is an addressable WS2812
 * rather than a plain LED -- a GPIO level will not light it, so we clock a
 * serial bit stream out over RMT via the led_strip component.
 */

#include "led.h"

#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "led";

/* Brightness when "on". Full scale is blinding at arm's length. */
#define LED_LEVEL 32

static led_strip_handle_t s_strip;
static bool s_led_on;

esp_err_t led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 init on GPIO %d failed: %s", LED_GPIO, esp_err_to_name(err));
        return err;
    }

    led_set(false);
    ESP_LOGI(TAG, "WS2812 LED ready on GPIO %d", LED_GPIO);
    return ESP_OK;
}

void led_set(bool on)
{
    s_led_on = on;
    if (on) {
        led_strip_set_pixel(s_strip, 0, LED_LEVEL, LED_LEVEL, LED_LEVEL);
        led_strip_refresh(s_strip);
    } else {
        led_strip_clear(s_strip);
    }
}

bool led_get(void)
{
    return s_led_on;
}
