/*
 * SEN55 lifecycle and sampling, wrapping the vendored Sensirion driver in
 * components/sen5x/.
 *
 * The driver returns fixed-point values with per-channel scale factors and uses
 * an all-ones sentinel for "unknown"; this file is where that convention is
 * converted into floats plus validity flags, so the Zigbee side never has to
 * know about it.
 */

#include "sensor.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sen5x_i2c.h"
#include "sensirion_i2c_hal.h"

static const char *TAG = "sen55";

/* "Unknown" sentinels, per sen5x_read_measured_values() docs. */
#define SEN5X_UNKNOWN_U16 0xFFFFu
#define SEN5X_UNKNOWN_S16 0x7FFF

esp_err_t sen55_init(void)
{
    sensirion_i2c_hal_init();

    /* A reset puts the sensor in a known state whether we are coming from a
     * cold boot or an ESP-only restart that left it measuring. */
    int16_t err = sen5x_device_reset();
    if (err) {
        ESP_LOGE(TAG, "device reset failed (%d) -- check wiring and pull-ups", err);
        return ESP_ERR_NOT_FOUND;
    }

    unsigned char product_name[32] = {0};
    unsigned char serial_number[32] = {0};
    if (sen5x_get_product_name(product_name, sizeof(product_name)) == 0 &&
        sen5x_get_serial_number(serial_number, sizeof(serial_number)) == 0) {
        ESP_LOGI(TAG, "found %s, serial %s", product_name, serial_number);
    }

    err = sen5x_start_measurement();
    if (err) {
        ESP_LOGE(TAG, "start measurement failed (%d)", err);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "measuring; VOC/NOx indices need ~1 min of warm-up");
    return ESP_OK;
}

esp_err_t sen55_read(sen55_reading_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    bool data_ready = false;
    int16_t err = sen5x_read_data_ready(&data_ready);
    if (err) {
        ESP_LOGW(TAG, "data-ready query failed (%d)", err);
        return ESP_FAIL;
    }
    if (!data_ready) {
        return ESP_ERR_NOT_FINISHED;
    }

    uint16_t pm1p0, pm2p5, pm4p0, pm10p0;
    int16_t humidity, temperature, voc, nox;
    err = sen5x_read_measured_values(&pm1p0, &pm2p5, &pm4p0, &pm10p0, &humidity, &temperature,
                                     &voc, &nox);
    if (err) {
        ESP_LOGW(TAG, "read failed (%d)", err);
        return ESP_FAIL;
    }

    /* All four PM channels come from the same optical measurement, so they are
     * unknown together; likewise RH and T share the RH/T element. */
    out->pm_valid = (pm2p5 != SEN5X_UNKNOWN_U16);
    out->rht_valid = (temperature != SEN5X_UNKNOWN_S16 && humidity != SEN5X_UNKNOWN_S16);
    /* Both indices run 1..500. A plain 0 is not the documented sentinel but
     * still means "the algorithm has not produced an output yet", which is what
     * the sensor emits for the first samples after a reset. */
    out->voc_valid = (voc != SEN5X_UNKNOWN_S16 && voc > 0);
    out->nox_valid = (nox != SEN5X_UNKNOWN_S16 && nox > 0);

    out->pm1p0 = pm1p0 / 10.0f;
    out->pm2p5 = pm2p5 / 10.0f;
    out->pm4p0 = pm4p0 / 10.0f;
    out->pm10p0 = pm10p0 / 10.0f;
    out->humidity = humidity / 100.0f;
    out->temperature = temperature / 200.0f;
    out->voc_index = voc / 10.0f;
    out->nox_index = nox / 10.0f;

    return ESP_OK;
}
