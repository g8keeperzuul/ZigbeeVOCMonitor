#pragma once

#include <stddef.h>

#include "esp_zigbee_core.h"
#include "sensor.h"

/*
 * Endpoint map. Temperature, humidity and PM2.5 have standard ZCL measurement
 * clusters and share one endpoint. The remaining SEN55 channels have no
 * standard cluster, so each gets its own Analog Input endpoint -- the Analog
 * Input cluster can only appear once per endpoint.
 */
#define ZB_EP_AIR_QUALITY 11 /* Temperature + Humidity + PM2.5 */
#define ZB_EP_VOC_INDEX   12
#define ZB_EP_NOX_INDEX   13
#define ZB_EP_PM1P0       14
#define ZB_EP_PM4P0       15
#define ZB_EP_PM10P0      16

/* Writes a ZCL character string (length-prefixed, not NUL-terminated) into dst.
 *
 * The buffer must have static storage duration: the stack keeps a pointer to
 * the value passed to esp_zb_*_cluster_add_attr() rather than copying it. */
void zb_zcl_string(char *dst, size_t dst_size, const char *src);

/* Appends the six sensor endpoints to ep_list. */
void zb_sensor_endpoints_add(esp_zb_ep_list_t *ep_list);

/* Installs default reporting intervals so the device reports even if the
 * coordinator never sends Configure Reporting.
 *
 * Must be called once the stack is actually up -- ZBOSS builds its reporting
 * table while processing startup, so calling this straight after
 * esp_zb_start() fails with ESP_ERR_NOT_FOUND on every attribute. Drive it
 * from a network-up signal instead.
 *
 * Returns the number of attributes that could not be configured; 0 on success,
 * so the caller can retry. */
size_t zb_sensor_reporting_init(void);

/* Logs whether every attribute this device publishes is actually present in the
 * stack. Call after esp_zb_start(); see the comment on the definition for how
 * to read the output. */
void zb_sensor_selftest(void);

/* Pushes a reading into the attribute store, which triggers reporting for any
 * value that moved past its reportable delta. Acquires the Zigbee lock, so call
 * it from a normal task, not from a Zigbee callback. */
void zb_sensor_publish(const sen55_reading_t *reading);
