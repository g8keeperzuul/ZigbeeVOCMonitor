/*
 * Zigbee representation of the SEN55.
 *
 * Endpoint 11 carries the three channels that have standard ZCL measurement
 * clusters (temperature, relative humidity, PM2.5) so coordinators discover
 * them without a custom converter. VOC index, NOx index, PM1.0, PM4.0 and PM10
 * have no standard cluster and are exposed as Analog Input endpoints, each
 * labelled via the cluster's Description attribute.
 */

#include "zb_sensor.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "zb_sensor";

/* Reporting: never more often than every 10 s, at least every 5 minutes even
 * if nothing changed, and immediately once a value moves by the delta below. */
#define REPORT_MIN_INTERVAL_S 10
#define REPORT_MAX_INTERVAL_S 300

#define TEMP_DELTA_HUNDREDTHS 50   /* 0.5 C  */
#define RH_DELTA_HUNDREDTHS   100  /* 1 %RH  */
#define PM_DELTA_UG           1.0f /* 1 ug/m3 */
#define INDEX_DELTA           5.0f /* 5 index points */

/* ZCL "invalid / not available" markers for the standard clusters. */
#define ZCL_TEMP_INVALID 0x8000
#define ZCL_RH_INVALID   0xFFFF

/* SEN55 operating ranges, published so the coordinator can sanity-check. */
#define TEMP_MIN_HUNDREDTHS (-1000) /* -10 C */
#define TEMP_MAX_HUNDREDTHS 5000    /*  50 C */
#define PM_MAX_UG           1000.0f
#define INDEX_MAX           500.0f

void zb_zcl_string(char *dst, size_t dst_size, const char *src)
{
    size_t len = strlen(src);
    if (len > dst_size - 1) {
        len = dst_size - 1;
    }
    dst[0] = (char)len;
    memcpy(dst + 1, src, len);
}

/* ------------------------------------------------------------------------- */
/* Endpoint construction                                                     */
/* ------------------------------------------------------------------------- */

/* One Analog Input endpoint per non-standard channel. The description buffers
 * are static so they outlive this call: the stack may hold onto the pointer for
 * string attributes rather than copying the bytes. */
static const struct {
    uint8_t endpoint;
    const char *description;
    float max_value;
} s_analog_inputs[] = {
    {ZB_EP_VOC_INDEX, "VOC Index", INDEX_MAX},
    {ZB_EP_NOX_INDEX, "NOx Index", INDEX_MAX},
    {ZB_EP_PM1P0, "PM1.0 ug/m3", PM_MAX_UG},
    {ZB_EP_PM4P0, "PM4.0 ug/m3", PM_MAX_UG},
    {ZB_EP_PM10P0, "PM10 ug/m3", PM_MAX_UG},
};

#define ANALOG_INPUT_COUNT (sizeof(s_analog_inputs) / sizeof(s_analog_inputs[0]))

/* Attribute values are referenced by pointer (esp_zb_zcl_attr_t.data_p), so
 * everything handed to add_attr needs to outlive the call that registers it. */
static char s_descriptions[ANALOG_INPUT_COUNT][20];
static float s_min_present[ANALOG_INPUT_COUNT];
static float s_max_present[ANALOG_INPUT_COUNT];

static esp_zb_attribute_list_t *identify_cluster(void)
{
    esp_zb_identify_cluster_cfg_t cfg = {
        .identify_time = 0,
    };
    return esp_zb_identify_cluster_create(&cfg);
}

static void air_quality_endpoint_add(esp_zb_ep_list_t *ep_list)
{
    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();

    esp_zb_temperature_meas_cluster_cfg_t temp_cfg = {
        .measured_value = ZCL_TEMP_INVALID,
        .min_value = TEMP_MIN_HUNDREDTHS,
        .max_value = TEMP_MAX_HUNDREDTHS,
    };
    esp_zb_humidity_meas_cluster_cfg_t rh_cfg = {
        .measured_value = ZCL_RH_INVALID,
        .min_value = 0,
        .max_value = 10000,
    };
    esp_zb_pm2_5_measurement_cluster_cfg_t pm_cfg = {
        .measured_value = 0.0f,
        .min_measured_value = 0.0f,
        .max_measured_value = PM_MAX_UG,
    };

    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(clusters, identify_cluster(),
                                                             ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_temperature_meas_cluster(
        clusters, esp_zb_temperature_meas_cluster_create(&temp_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_humidity_meas_cluster(
        clusters, esp_zb_humidity_meas_cluster_create(&rh_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_pm2_5_measurement_cluster(
        clusters, esp_zb_pm2_5_measurement_cluster_create(&pm_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = ZB_EP_AIR_QUALITY,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };
    ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(ep_list, clusters, ep_cfg));
}

/* Description / MinPresentValue / MaxPresentValue are optional in the Analog
 * Input cluster, and add_attr rejects anything the stack does not implement.
 * Losing a label is not worth aborting the boot over, so warn and carry on. */
static void analog_input_add_optional_attr(esp_zb_attribute_list_t *ai, uint16_t attr_id,
                                           void *value, const char *what)
{
    esp_err_t err = esp_zb_analog_input_cluster_add_attr(ai, attr_id, value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "analog input %s attribute not added: %s", what, esp_err_to_name(err));
    }
}

static void analog_input_endpoint_add(esp_zb_ep_list_t *ep_list, size_t index)
{
    esp_zb_analog_input_cluster_cfg_t ai_cfg = {
        .out_of_service = false,
        .present_value = 0.0f,
        .status_flags = 0,
    };
    esp_zb_attribute_list_t *ai = esp_zb_analog_input_cluster_create(&ai_cfg);

    zb_zcl_string(s_descriptions[index], sizeof(s_descriptions[index]),
                  s_analog_inputs[index].description);
    analog_input_add_optional_attr(ai, ESP_ZB_ZCL_ATTR_ANALOG_INPUT_DESCRIPTION_ID,
                                   s_descriptions[index], "description");

    s_min_present[index] = 0.0f;
    s_max_present[index] = s_analog_inputs[index].max_value;
    analog_input_add_optional_attr(ai, ESP_ZB_ZCL_ATTR_ANALOG_INPUT_MIN_PRESENT_VALUE_ID,
                                   &s_min_present[index], "min present value");
    analog_input_add_optional_attr(ai, ESP_ZB_ZCL_ATTR_ANALOG_INPUT_MAX_PRESENT_VALUE_ID,
                                   &s_max_present[index], "max present value");

    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(clusters, identify_cluster(),
                                                             ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(
        esp_zb_cluster_list_add_analog_input_cluster(clusters, ai, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = s_analog_inputs[index].endpoint,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };
    ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(ep_list, clusters, ep_cfg));
}

void zb_sensor_endpoints_add(esp_zb_ep_list_t *ep_list)
{
    air_quality_endpoint_add(ep_list);
    for (size_t i = 0; i < ANALOG_INPUT_COUNT; i++) {
        analog_input_endpoint_add(ep_list, i);
    }
}

/* ------------------------------------------------------------------------- */
/* Reporting                                                                 */
/* ------------------------------------------------------------------------- */

/* Every attribute this device publishes, in one place: reporting setup and the
 * boot-time self-check both walk this table. */
static const struct {
    uint8_t endpoint;
    uint16_t cluster_id;
    uint16_t attr_id;
    const char *label;
    union esp_zb_zcl_attr_var_u delta;
} s_published[] = {
    {ZB_EP_AIR_QUALITY, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
     ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, "temperature",
     {.s16 = TEMP_DELTA_HUNDREDTHS}},
    {ZB_EP_AIR_QUALITY, ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
     ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID, "humidity",
     {.u16 = RH_DELTA_HUNDREDTHS}},
    {ZB_EP_AIR_QUALITY, ESP_ZB_ZCL_CLUSTER_ID_PM2_5_MEASUREMENT,
     ESP_ZB_ZCL_ATTR_PM2_5_MEASUREMENT_MEASURED_VALUE_ID, "pm2.5", {.f32 = PM_DELTA_UG}},
    {ZB_EP_VOC_INDEX, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
     ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, "voc index", {.f32 = INDEX_DELTA}},
    {ZB_EP_NOX_INDEX, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
     ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, "nox index", {.f32 = INDEX_DELTA}},
    {ZB_EP_PM1P0, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
     ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, "pm1.0", {.f32 = PM_DELTA_UG}},
    {ZB_EP_PM4P0, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
     ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, "pm4.0", {.f32 = PM_DELTA_UG}},
    {ZB_EP_PM10P0, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
     ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, "pm10", {.f32 = PM_DELTA_UG}},
};

#define PUBLISHED_COUNT (sizeof(s_published) / sizeof(s_published[0]))

size_t zb_sensor_reporting_init(void)
{
    size_t failed = 0;

    for (size_t i = 0; i < PUBLISHED_COUNT; i++) {
        esp_zb_zcl_reporting_info_t info = {
            .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
            .ep = s_published[i].endpoint,
            .cluster_id = s_published[i].cluster_id,
            .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            .attr_id = s_published[i].attr_id,
            .flags = 0,
            .dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID,
            /* Must be 0xFFFF, not 0. This is part of the key the stack uses to
             * find the attribute; leaving it zero means "manufacturer code 0"
             * and the lookup fails with ESP_ERR_NOT_FOUND. */
            .manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
            .u.send_info = {
                .min_interval = REPORT_MIN_INTERVAL_S,
                .max_interval = REPORT_MAX_INTERVAL_S,
                .def_min_interval = REPORT_MIN_INTERVAL_S,
                .def_max_interval = REPORT_MAX_INTERVAL_S,
                .delta = s_published[i].delta,
            },
        };

        esp_err_t err = esp_zb_zcl_update_reporting_info(&info);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "reporting setup failed for %s (ep %d cluster 0x%04x): %s",
                     s_published[i].label, s_published[i].endpoint, s_published[i].cluster_id,
                     esp_err_to_name(err));
            failed++;
        }
    }

    return failed;
}

/* ------------------------------------------------------------------------- */
/* Self-check                                                                */
/* ------------------------------------------------------------------------- */

/*
 * Prints, for every attribute we intend to publish, whether the stack actually
 * has it. This separates the two ways the sensor data can fail to show up:
 *
 *   MISSING  -> the endpoint never made it into the stack, so this is a
 *               firmware problem and no coordinator will ever see it.
 *   ok       -> the device side is fine; if the coordinator still shows only
 *               the light, it is serving a cached interview from before these
 *               endpoints existed. Re-interview or re-pair the device.
 *
 * "not reportable" on an otherwise-ok attribute means writes will land in the
 * attribute store but never go on air by themselves.
 */
void zb_sensor_selftest(void)
{
    size_t missing = 0;

    for (size_t i = 0; i < PUBLISHED_COUNT; i++) {
        esp_zb_zcl_attr_t *attr =
            esp_zb_zcl_get_attribute(s_published[i].endpoint, s_published[i].cluster_id,
                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, s_published[i].attr_id);
        if (!attr) {
            ESP_LOGE(TAG, "ep %2d cluster 0x%04x attr 0x%04x (%s): MISSING", s_published[i].endpoint,
                     s_published[i].cluster_id, s_published[i].attr_id, s_published[i].label);
            missing++;
            continue;
        }
        ESP_LOGI(TAG, "ep %2d cluster 0x%04x attr 0x%04x (%s): ok, type 0x%02x, access 0x%02x%s",
                 s_published[i].endpoint, s_published[i].cluster_id, s_published[i].attr_id,
                 s_published[i].label, attr->type, attr->access,
                 (attr->access & ESP_ZB_ZCL_ATTR_ACCESS_REPORTING) ? "" : " NOT REPORTABLE");
    }

    if (missing) {
        ESP_LOGE(TAG, "%u of %u sensor attributes missing -- endpoints did not register",
                 (unsigned)missing, (unsigned)PUBLISHED_COUNT);
    } else {
        ESP_LOGI(TAG,
                 "all %u sensor attributes registered; if the coordinator shows only the "
                 "light, it is using a cached interview -- re-interview or re-pair",
                 (unsigned)PUBLISHED_COUNT);
    }
}

/* ------------------------------------------------------------------------- */
/* Publishing                                                                */
/* ------------------------------------------------------------------------- */

static void set_attr(uint8_t endpoint, uint16_t cluster_id, uint16_t attr_id, void *value)
{
    esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(
        endpoint, cluster_id, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr_id, value, false);
    if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "write to ep %d cluster 0x%04x attr 0x%04x failed: 0x%x", endpoint,
                 cluster_id, attr_id, status);
    }
}

static void set_analog_input(uint8_t endpoint, float value)
{
    set_attr(endpoint, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
             ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, &value);
}

void zb_sensor_publish(const sen55_reading_t *reading)
{
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(1000))) {
        ESP_LOGW(TAG, "could not take the Zigbee lock, dropping this sample");
        return;
    }

    if (reading->rht_valid) {
        /* SEN55 temperature is degrees C; ZCL wants hundredths. */
        int16_t temp = (int16_t)lroundf(reading->temperature * 100.0f);
        uint16_t rh = (uint16_t)lroundf(reading->humidity * 100.0f);
        set_attr(ZB_EP_AIR_QUALITY, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                 ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, &temp);
        set_attr(ZB_EP_AIR_QUALITY, ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                 ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID, &rh);
    }

    if (reading->pm_valid) {
        /* The ZCL concentration clusters nominally carry a fraction of one, but
         * every real PM2.5 device -- and Zigbee2MQTT's converter -- treats this
         * as ug/m3, so send the raw concentration. */
        float pm2p5 = reading->pm2p5;
        set_attr(ZB_EP_AIR_QUALITY, ESP_ZB_ZCL_CLUSTER_ID_PM2_5_MEASUREMENT,
                 ESP_ZB_ZCL_ATTR_PM2_5_MEASUREMENT_MEASURED_VALUE_ID, &pm2p5);

        set_analog_input(ZB_EP_PM1P0, reading->pm1p0);
        set_analog_input(ZB_EP_PM4P0, reading->pm4p0);
        set_analog_input(ZB_EP_PM10P0, reading->pm10p0);
    }

    /* Left untouched while warming up, so the coordinator keeps showing the
     * last good index rather than a bogus zero. */
    if (reading->voc_valid) {
        set_analog_input(ZB_EP_VOC_INDEX, reading->voc_index);
    }
    if (reading->nox_valid) {
        set_analog_input(ZB_EP_NOX_INDEX, reading->nox_index);
    }

    esp_zb_lock_release();
}
