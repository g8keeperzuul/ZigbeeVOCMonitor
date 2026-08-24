#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * SEN55 readings in engineering units.
 *
 * The VOC and NOx algorithms need a warm-up before they output anything: for
 * roughly the first minute of a measurement the sensor reports "unknown" for
 * those channels, which surfaces here as voc_valid / nox_valid being false.
 */
typedef struct {
    float pm1p0;       /* ug/m3 */
    float pm2p5;       /* ug/m3 */
    float pm4p0;       /* ug/m3 */
    float pm10p0;      /* ug/m3 */
    float humidity;    /* %RH */
    float temperature; /* degrees C */
    float voc_index;   /* 1..500 index, valid only if voc_valid */
    float nox_index;   /* 1..500 index, valid only if nox_valid */
    bool pm_valid;
    bool rht_valid;
    bool voc_valid;
    bool nox_valid;
} sen55_reading_t;

/* Brings up I2C, resets the sensor and starts continuous measurement.
 * Logs the product name and serial number on success. */
esp_err_t sen55_init(void);

/* Reads one sample. Returns ESP_ERR_NOT_FINISHED if the sensor has no new data
 * yet (it produces one sample per second). */
esp_err_t sen55_read(sen55_reading_t *out);
