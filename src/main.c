/*
 * ESP32-C6 Zigbee air quality monitor.
 *
 * Joins an already-running Zigbee coordinator (Zigbee2MQTT, ZHA, deCONZ, a
 * Hue/SmartThings hub, ...) as a Router via BDB network steering, then exposes:
 *
 *   endpoint 10       On/Off Light -- drives the LED on GPIO 8
 *   endpoints 11..16  SEN55 readings (see zb_sensor.h for the endpoint map)
 *
 *     pio run -t upload -t monitor
 *
 * To let the device join, put the coordinator into "permit join" mode and
 * reset/power-cycle this board.
 */

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"

#include "display.h"
#include "led.h"
#include "sensor.h"
#include "zb_sensor.h"

static const char *TAG = "zb_air";

/* Endpoint the On/Off Light lives on. Any value in 1..240 works. */
#define HA_LIGHT_ENDPOINT 10

/* Channels to scan for the existing coordinator. The default mask covers the
 * whole 2.4 GHz 802.15.4 band (channels 11-26). Narrow it to the coordinator's
 * channel (e.g. `1 << 15`) to make joining noticeably faster. */
#define ZB_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

/* How many children this router will accept. */
#define ZB_MAX_CHILDREN 10

/* Delay before retrying a failed join, and how many times to try. */
#define STEERING_RETRY_DELAY_MS 5000
#define STEERING_MAX_RETRIES    100

/* Reported in the Basic cluster; this is what shows up in the coordinator UI.
 * Static, because the stack keeps a pointer to the string rather than a copy. */
#define MANUFACTURER_NAME "Espressif"
#define MODEL_IDENTIFIER  "ESP32C6.AirQuality"

static char s_manufacturer[sizeof(MANUFACTURER_NAME) + 1];
static char s_model[sizeof(MODEL_IDENTIFIER) + 1];

static uint8_t s_steering_retries;

/* ------------------------------------------------------------------------- */
/* Endpoint / cluster setup                                                  */
/* ------------------------------------------------------------------------- */

static void light_endpoint_add(esp_zb_ep_list_t *ep_list)
{
    esp_zb_on_off_light_cfg_t light_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    esp_zb_cluster_list_t *clusters = esp_zb_on_off_light_clusters_create(&light_cfg);

    /* Overwrite the placeholder Basic-cluster identity strings. */
    zb_zcl_string(s_manufacturer, sizeof(s_manufacturer), MANUFACTURER_NAME);
    zb_zcl_string(s_model, sizeof(s_model), MODEL_IDENTIFIER);

    esp_zb_attribute_list_t *basic =
        esp_zb_cluster_list_get_cluster(clusters, ESP_ZB_ZCL_CLUSTER_ID_BASIC,
                                        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(
        basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, s_manufacturer));
    ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(
        basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, s_model));

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = HA_LIGHT_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID,
        .app_device_version = 0,
    };
    ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(ep_list, clusters, ep_cfg));
}

/* ------------------------------------------------------------------------- */
/* Incoming commands                                                         */
/* ------------------------------------------------------------------------- */

/*
 * The stack has already written the new value into the attribute store by the
 * time we get here -- our job is only to make the hardware match. This covers
 * On, Off *and* Toggle, since all three land as a write of the OnOff attribute.
 */
static esp_err_t on_set_attribute(const esp_zb_zcl_set_attr_value_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "empty message");
    ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG,
                        TAG, "attribute write failed with status 0x%x", message->info.status);

    if (message->info.dst_endpoint != HA_LIGHT_ENDPOINT ||
        message->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
        ESP_LOGD(TAG, "ignoring write to ep %d cluster 0x%04x",
                 message->info.dst_endpoint, message->info.cluster);
        return ESP_OK;
    }

    if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID &&
        message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL &&
        message->attribute.data.value != NULL) {
        bool on = *(bool *)message->attribute.data.value;
        led_set(on);
        ESP_LOGI(TAG, "LED %s", on ? "ON" : "OFF");
    }

    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                  const void *message)
{
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        return on_set_attribute((const esp_zb_zcl_set_attr_value_message_t *)message);
    default:
        ESP_LOGD(TAG, "unhandled Zigbee action 0x%x", callback_id);
        return ESP_OK;
    }
}

/* ------------------------------------------------------------------------- */
/* Commissioning / network signals                                           */
/* ------------------------------------------------------------------------- */

/*
 * Reporting can only be configured once ZBOSS has finished starting, which is
 * later than esp_zb_start() returning -- see zb_sensor_reporting_init(). It is
 * driven from the signals below and retried until it takes.
 */
static bool s_reporting_ready;

static void configure_reporting_once(void)
{
    if (s_reporting_ready) {
        return;
    }
    size_t failed = zb_sensor_reporting_init();
    if (failed == 0) {
        s_reporting_ready = true;
        ESP_LOGI(TAG, "attribute reporting configured");
    } else {
        ESP_LOGW(TAG, "%u attributes not configured for reporting; will retry", (unsigned)failed);
    }
}

static void steering_retry_cb(uint8_t param)
{
    (void)param;
    ESP_LOGI(TAG, "retrying network steering (%u/%u)", s_steering_retries, STEERING_MAX_RETRIES);
    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
}

static void start_steering(void)
{
    ESP_LOGI(TAG, "searching for a coordinator to join...");
    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
}

static void schedule_steering_retry(void)
{
    if (s_steering_retries >= STEERING_MAX_RETRIES) {
        ESP_LOGE(TAG, "giving up after %u join attempts; reset the board to try again",
                 s_steering_retries);
        return;
    }
    s_steering_retries++;
    esp_zb_scheduler_alarm(steering_retry_cb, 0, STEERING_RETRY_DELAY_MS);
}

/* Called by the stack; the name is fixed by the SDK. */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    esp_zb_app_signal_type_t sig_type = *signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "stack initialized, starting BDB commissioning");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
        /* No credentials in zb_storage yet -- this is a fresh device. */
        if (err_status == ESP_OK) {
            s_steering_retries = 0;
            start_steering();
        } else {
            ESP_LOGE(TAG, "stack init failed: %s", esp_err_to_name(err_status));
        }
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        /* Credentials were restored from zb_storage. */
        if (err_status == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                s_steering_retries = 0;
                start_steering();
            } else {
                ESP_LOGI(TAG, "rejoined network, PAN 0x%04hx, short addr 0x%04hx",
                         esp_zb_get_pan_id(), esp_zb_get_short_address());
                configure_reporting_once();
            }
        } else {
            ESP_LOGE(TAG, "reboot signal error: %s", esp_err_to_name(err_status));
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t ext_pan_id;
            esp_zb_get_extended_pan_id(ext_pan_id);
            ESP_LOGI(TAG,
                     "joined as Router: ext PAN %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, "
                     "PAN 0x%04hx, channel %d, short addr 0x%04hx",
                     ext_pan_id[7], ext_pan_id[6], ext_pan_id[5], ext_pan_id[4],
                     ext_pan_id[3], ext_pan_id[2], ext_pan_id[1], ext_pan_id[0],
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(),
                     esp_zb_get_short_address());
            s_steering_retries = 0;
            configure_reporting_once();
        } else {
            ESP_LOGW(TAG, "no network joined (%s) -- is the coordinator permitting joins?",
                     esp_err_to_name(err_status));
            schedule_steering_retry();
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        /* Removed by the coordinator. Clear local state and look for a network. */
        ESP_LOGW(TAG, "left the network");
        led_set(false);
        s_steering_retries = 0;
        start_steering();
        break;

    default:
        ESP_LOGD(TAG, "signal %s (0x%x), status: %s", esp_zb_zdo_signal_to_string(sig_type),
                 sig_type, esp_err_to_name(err_status));
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Sensor sampling                                                           */
/* ------------------------------------------------------------------------- */

/*
 * The SEN55 produces one sample per second, but sampling that fast buys
 * nothing: the reportable deltas in zb_sensor.c decide what actually goes on
 * air, and the VOC/NOx algorithms integrate over minutes anyway.
 */
#define SENSOR_PERIOD_MS 10000

static void sensor_task(void *pvParameters)
{
    (void)pvParameters;

    if (sen55_init() != ESP_OK) {
        ESP_LOGE(TAG, "SEN55 not responding; continuing without it");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SENSOR_PERIOD_MS));

        sen55_reading_t reading;
        esp_err_t err = sen55_read(&reading);
        if (err == ESP_ERR_NOT_FINISHED) {
            continue; /* No new sample yet. */
        }
        if (err != ESP_OK) {
            continue; /* sen55_read() already logged the reason. */
        }

        ESP_LOGI(TAG,
                 "PM1.0 %.1f  PM2.5 %.1f  PM4.0 %.1f  PM10 %.1f ug/m3  "
                 "%.2f C  %.2f %%RH  VOC %.0f  NOx %.0f",
                 reading.pm1p0, reading.pm2p5, reading.pm4p0, reading.pm10p0,
                 reading.temperature, reading.humidity,
                 reading.voc_valid ? reading.voc_index : -1.0f,
                 reading.nox_valid ? reading.nox_index : -1.0f);

        zb_sensor_publish(&reading);
        display_publish(&reading);
    }
}

/* ------------------------------------------------------------------------- */
/* Entry points                                                              */
/* ------------------------------------------------------------------------- */

static void esp_zb_task(void *pvParameters)
{
    (void)pvParameters;

    esp_zb_cfg_t zb_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg = {
            .max_children = ZB_MAX_CHILDREN,
        },
    };
    esp_zb_init(&zb_cfg);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    light_endpoint_add(ep_list);
    zb_sensor_endpoints_add(ep_list);

    ESP_ERROR_CHECK(esp_zb_device_register(ep_list));
    esp_zb_core_action_handler_register(zb_action_handler);
    ESP_ERROR_CHECK(esp_zb_set_primary_network_channel_set(ZB_PRIMARY_CHANNEL_MASK));

    ESP_ERROR_CHECK(esp_zb_start(false));

    /* Reporting is configured later, from the network-up signals -- the
     * reporting table does not exist yet at this point. */
    zb_sensor_selftest();

    /* I2C is blocking and the ZBOSS loop below never returns, so the sensor
     * gets its own task. */
    xTaskCreate(sensor_task, "sen55", 5120, NULL, 4, NULL);

    /* Same reasoning for the e-Paper: a refresh blocks for about a second. */
    if (display_start() != ESP_OK) {
        ESP_LOGE(TAG, "could not start the display task");
    }

    esp_zb_stack_main_loop();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(led_init());

    /* The C6 has an on-chip 802.15.4 radio and we are not acting as an RCP for
     * a separate host, so both links are "native"/"none". */
    esp_zb_platform_config_t platform_cfg = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform_cfg));

    ESP_LOGI(TAG, "starting Zigbee Router: LED on GPIO %d, SEN55 on I2C, e-Paper on SPI2",
             LED_GPIO);

    /* The ZBOSS main loop is blocking, so it gets its own task. */
    xTaskCreate(esp_zb_task, "zigbee_main", 4096, NULL, 5, NULL);
}
