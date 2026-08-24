/*
 * Waveshare 2.13" e-Paper (V4) readout.
 *
 * Refresh policy, which is mostly dictated by what the panel tolerates:
 *
 *   - Never more often than once a minute.
 *   - Only when the screen would actually look different. The comparison is on
 *     the formatted strings rather than on float thresholds, so "would the
 *     pixels change" is answered exactly rather than approximated.
 *   - Always a full refresh, and the panel sleeps in between. Partial refresh
 *     would be gentler per update, but it needs the previous image still held
 *     in panel RAM, which means leaving the panel powered -- and Waveshare are
 *     explicit that a panel held in a powered high-voltage state is damaged
 *     beyond repair. Sleeping wins; the fast refresh path costs ~1 s and one
 *     flicker, and being a true full refresh it accumulates no ghosting.
 *   - At least once every 24 h regardless, per the image-sticking guidance.
 */

#include "display.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "DEV_Config.h"
#include "EPD_2in13_V4.h"
#include "GUI_Paint.h"

#include "display_layout.h"

static const char *TAG = "display";

#define REFRESH_MIN_INTERVAL_US (60ULL * 1000000ULL)
#define REFRESH_MAX_INTERVAL_US (24ULL * 3600ULL * 1000000ULL)

/* display_layout.h declares the panel geometry independently of the driver so
 * it can be built on a host; make sure the two never drift apart. */
_Static_assert(DISPLAY_PANEL_W == EPD_2in13_V4_WIDTH, "panel width mismatch");
_Static_assert(DISPLAY_PANEL_H == EPD_2in13_V4_HEIGHT, "panel height mismatch");

static QueueHandle_t s_mailbox;
static UBYTE *s_framebuffer;

static void format_screen(const sen55_reading_t *r, display_fields_t *out)
{
    /* Zero the whole struct, padding included: the change check is a memcmp of
     * two of these, so stale padding bytes would show up as a false difference. */
    memset(out, 0, sizeof(*out));

    if (r->voc_valid) {
        snprintf(out->voc, sizeof(out->voc), "%.0f", r->voc_index);
    } else {
        snprintf(out->voc, sizeof(out->voc), "--");
    }

    if (!r->pm_valid) {
        snprintf(out->pm25, sizeof(out->pm25), "-- ug/m3");
    } else if (r->pm2p5 >= 100.0f) {
        /* Drop the decimal once it would push the string past the cell. */
        snprintf(out->pm25, sizeof(out->pm25), "%.0f ug/m3", r->pm2p5);
    } else {
        snprintf(out->pm25, sizeof(out->pm25), "%.1f ug/m3", r->pm2p5);
    }

    if (r->rht_valid) {
        snprintf(out->temp, sizeof(out->temp), "%.1f C", r->temperature);
        /* "100.0 %RH" is exactly as wide as its cell and collides with the
         * frame, so saturated humidity loses the decimal. */
        snprintf(out->humidity, sizeof(out->humidity),
                 r->humidity >= 99.95f ? "%.0f %%RH" : "%.1f %%RH", r->humidity);
    } else {
        snprintf(out->temp, sizeof(out->temp), "-- C");
        snprintf(out->humidity, sizeof(out->humidity), "-- %%RH");
    }

}


/* Wake, draw, sleep. The panel must be re-initialised on every wake -- data sent
 * while it is asleep is discarded. */
static void push_to_panel(void)
{
    EPD_2in13_V4_Init_Fast();
    EPD_2in13_V4_Display_Fast(s_framebuffer);
    EPD_2in13_V4_Sleep();
}

static esp_err_t panel_start(void)
{
    if (DEV_Module_Init() != 0) {
        return ESP_FAIL;
    }

    s_framebuffer = heap_caps_malloc(DISPLAY_FRAMEBUFFER_BYTES, MALLOC_CAP_DMA);
    if (!s_framebuffer) {
        ESP_LOGE(TAG, "no DMA memory for a %d byte framebuffer", DISPLAY_FRAMEBUFFER_BYTES);
        DEV_Module_Exit();
        return ESP_ERR_NO_MEM;
    }

    /* A full clear on the first power-up, so we never inherit whatever was
     * latched on the panel before a reset. */
    EPD_2in13_V4_Init();
    EPD_2in13_V4_Clear();

    Paint_NewImage(s_framebuffer, DISPLAY_PANEL_W, DISPLAY_PANEL_H, ROTATE_90, WHITE);
    display_fields_t placeholder = {0};
    snprintf(placeholder.voc, sizeof(placeholder.voc), "--");
    snprintf(placeholder.pm25, sizeof(placeholder.pm25), "-- ug/m3");
    snprintf(placeholder.temp, sizeof(placeholder.temp), "-- C");
    snprintf(placeholder.humidity, sizeof(placeholder.humidity), "-- %%RH");
    display_layout_render(s_framebuffer, &placeholder);
    push_to_panel();

    ESP_LOGI(TAG, "e-Paper ready, %dx%d landscape, %d byte framebuffer", DISPLAY_CANVAS_W, DISPLAY_CANVAS_H,
             DISPLAY_FRAMEBUFFER_BYTES);
    return ESP_OK;
}

static void display_task(void *pvParameters)
{
    (void)pvParameters;

    if (panel_start() != ESP_OK) {
        ESP_LOGE(TAG, "e-Paper not available; continuing without it");
        vTaskDelete(NULL);
        return;
    }

    display_fields_t drawn = {0};
    sen55_reading_t latest;
    bool have_reading = false;
    int64_t last_refresh_us = esp_timer_get_time();

    for (;;) {
        if (xQueueReceive(s_mailbox, &latest, pdMS_TO_TICKS(1000)) == pdTRUE) {
            have_reading = true;
        }
        if (!have_reading) {
            continue;
        }

        int64_t since_us = esp_timer_get_time() - last_refresh_us;
        if (since_us < (int64_t)REFRESH_MIN_INTERVAL_US) {
            continue;
        }

        display_fields_t next;
        format_screen(&latest, &next);

        bool changed = memcmp(&next, &drawn, sizeof(next)) != 0;
        bool stale = since_us >= (int64_t)REFRESH_MAX_INTERVAL_US;
        if (!changed && !stale) {
            continue;
        }

        display_layout_render(s_framebuffer, &next);
        push_to_panel();

        drawn = next;
        last_refresh_us = esp_timer_get_time();
        ESP_LOGI(TAG, "refreshed (%s): VOC %s | %s | %s | %s",
                 stale && !changed ? "24h" : "changed", next.voc, next.pm25, next.temp,
                 next.humidity);
    }
}

void display_publish(const sen55_reading_t *reading)
{
    if (!s_mailbox || !reading) {
        return;
    }
    xQueueOverwrite(s_mailbox, reading);
}

esp_err_t display_start(void)
{
    s_mailbox = xQueueCreate(1, sizeof(sen55_reading_t));
    if (!s_mailbox) {
        return ESP_ERR_NO_MEM;
    }

    /* Below the sensor task and the Zigbee stack: a one-second blocking refresh
     * must never delay sampling or the radio. */
    if (xTaskCreate(display_task, "epaper", 4096, NULL, 3, NULL) != pdPASS) {
        vQueueDelete(s_mailbox);
        s_mailbox = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
