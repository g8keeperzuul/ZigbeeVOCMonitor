/*
 * Zigbee2MQTT external converter for the ESP32-C6 SEN55 air quality monitor.
 *
 * Install: drop this file into the `external_converters/` folder that sits
 * beside configuration.yaml in the Zigbee2MQTT data directory, then restart.
 * Files there are auto-loaded -- Zigbee2MQTT 2.x removed the
 * `external_converters:` configuration key. On 2.11.0 and later you also need
 * `enable_external_js: true` in configuration.yaml; earlier 2.x does not.
 *
 * Without this, Zigbee2MQTT falls back to an auto-generated definition, which
 * publishes endpoint-suffixed keys (temperature_11, state_10, ...), skips the
 * Analog Input channels entirely, and does not bind the measurement clusters --
 * so nothing ever reports and every value stays null.
 *
 * Temperature, humidity and PM2.5 arrive on standard clusters. The other five
 * channels are bare Analog Input clusters, which carry no hint of what they
 * measure; the endpoint number is the only thing distinguishing them, so the
 * converter below maps endpoint -> property name. Keep ENDPOINTS in step with
 * zb_sensor.h.
 */

import * as fz from 'zigbee-herdsman-converters/converters/fromZigbee';
import * as tz from 'zigbee-herdsman-converters/converters/toZigbee';
import * as exposes from 'zigbee-herdsman-converters/lib/exposes';
import * as reporting from 'zigbee-herdsman-converters/lib/reporting';

const e = exposes.presets;
const ea = exposes.access;

/* Analog Input endpoint -> published property. Mirrors zb_sensor.h. */
const ENDPOINTS = {
    12: 'voc_index',
    13: 'nox_index',
    14: 'pm1',
    15: 'pm4',
    16: 'pm10',
};

const REPORTING = {min: 10, max: 300, change: 1};

const fzAnalogInput = {
    cluster: 'genAnalogInput',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        const property = ENDPOINTS[msg.endpoint.ID];
        if (property === undefined || msg.data.presentValue === undefined) {
            return;
        }
        return {[property]: Math.round(msg.data.presentValue * 10) / 10};
    },
};

export default {
    zigbeeModel: ['ESP32C6.AirQuality'],
    model: 'ESP32C6.AirQuality',
    vendor: 'Espressif',
    description: 'ESP32-C6 Zigbee router with Sensirion SEN55 and a status LED',

    fromZigbee: [fz.on_off, fz.temperature, fz.humidity, fz.pm25, fzAnalogInput],
    toZigbee: [tz.on_off],

    /* The LED is the only writable thing, so it is the default target for
     * commands. Not paired with meta.multiEndpoint on purpose: the standard
     * measurements should publish as plain `temperature` etc., without an
     * endpoint suffix. */
    endpoint: () => ({default: 10}),

    exposes: [
        e.switch(),
        e.temperature(),
        e.humidity(),
        e.pm25(),
        exposes
            .numeric('voc_index', ea.STATE)
            .withValueMin(1)
            .withValueMax(500)
            .withDescription('Sensirion VOC Index, 100 is the running average'),
        exposes
            .numeric('nox_index', ea.STATE)
            .withValueMin(1)
            .withValueMax(500)
            .withDescription('Sensirion NOx Index, 1 means no NOx detected'),
        exposes.numeric('pm1', ea.STATE).withUnit('µg/m³').withDescription('PM1.0'),
        exposes.numeric('pm4', ea.STATE).withUnit('µg/m³').withDescription('PM4.0'),
        exposes.numeric('pm10', ea.STATE).withUnit('µg/m³').withDescription('PM10'),
    ],

    configure: async (device, coordinatorEndpoint) => {
        const light = device.getEndpoint(10);
        await reporting.bind(light, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(light);

        const airQuality = device.getEndpoint(11);
        await reporting.bind(airQuality, coordinatorEndpoint, [
            'msTemperatureMeasurement',
            'msRelativeHumidity',
            'pm25Measurement',
        ]);
        await reporting.temperature(airQuality);
        await reporting.humidity(airQuality);
        await airQuality.configureReporting('pm25Measurement', [
            {
                attribute: 'measuredValue',
                minimumReportInterval: REPORTING.min,
                maximumReportInterval: REPORTING.max,
                reportableChange: REPORTING.change,
            },
        ]);

        for (const id of Object.keys(ENDPOINTS)) {
            const endpoint = device.getEndpoint(Number(id));
            await reporting.bind(endpoint, coordinatorEndpoint, ['genAnalogInput']);
            await endpoint.configureReporting('genAnalogInput', [
                {
                    attribute: 'presentValue',
                    minimumReportInterval: REPORTING.min,
                    maximumReportInterval: REPORTING.max,
                    reportableChange: REPORTING.change,
                },
            ]);
        }
    },
};
