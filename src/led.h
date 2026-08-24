#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* GPIO the onboard addressable WS2812 LED is wired to. */
#define LED_GPIO 8

esp_err_t led_init(void);

/* Drive the LED. Safe to call from the Zigbee stack task. */
void led_set(bool on);

bool led_get(void);
