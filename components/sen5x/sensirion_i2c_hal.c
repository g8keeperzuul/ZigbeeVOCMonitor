/*
 * ESP-IDF implementation of the Sensirion I2C HAL.
 *
 * The upstream driver ships an esp32 sample under sample-implementations/, but
 * it is written against esp-idf-lib's i2c_dev_t rather than plain ESP-IDF. This
 * implementation targets the IDF 5.x `i2c_master` driver directly.
 *
 * The HAL entry points take the 7-bit address on every call, while i2c_master
 * wants a device handle bound to an address up front. Only one address is ever
 * used (the SEN5x lives at 0x69), so the handle is created lazily on first use
 * and reused; a different address simply gets its own handle on the same bus.
 */

#include "sensirion_i2c_hal.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sen5x_hal";

#ifndef SEN5X_I2C_PORT
#define SEN5X_I2C_PORT I2C_NUM_0
#endif
#ifndef SEN5X_I2C_SDA_GPIO
#define SEN5X_I2C_SDA_GPIO 6
#endif
#ifndef SEN5X_I2C_SCL_GPIO
#define SEN5X_I2C_SCL_GPIO 7
#endif

/* The SEN5x tops out at 100 kHz. */
#define SEN5X_I2C_FREQ_HZ 100000

#define SEN5X_I2C_TIMEOUT_MS 1000

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_dev_addr;

void sensirion_i2c_hal_init(void)
{
    if (s_bus) {
        return;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = SEN5X_I2C_PORT,
        .sda_io_num = SEN5X_I2C_SDA_GPIO,
        .scl_io_num = SEN5X_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* A fallback only. The SEN5x datasheet asks for external pull-ups
         * (10k to 3V3); the internal ~45k is too weak for a long harness. */
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed (SDA %d, SCL %d): %s", SEN5X_I2C_SDA_GPIO,
                 SEN5X_I2C_SCL_GPIO, esp_err_to_name(err));
        s_bus = NULL;
        return;
    }

    ESP_LOGI(TAG, "i2c ready on SDA %d / SCL %d @ %d kHz", SEN5X_I2C_SDA_GPIO,
             SEN5X_I2C_SCL_GPIO, SEN5X_I2C_FREQ_HZ / 1000);
}

void sensirion_i2c_hal_free(void)
{
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        s_dev_addr = 0;
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
}

/* Returns the device handle for `address`, creating it on first use. */
static i2c_master_dev_handle_t hal_device(uint8_t address)
{
    if (!s_bus) {
        return NULL;
    }
    if (s_dev && s_dev_addr == address) {
        return s_dev;
    }
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = SEN5X_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adding device 0x%02x failed: %s", address, esp_err_to_name(err));
        s_dev = NULL;
        return NULL;
    }
    s_dev_addr = address;
    return s_dev;
}

int8_t sensirion_i2c_hal_read(uint8_t address, uint8_t *data, uint16_t count)
{
    i2c_master_dev_handle_t dev = hal_device(address);
    if (!dev) {
        return -1;
    }
    esp_err_t err = i2c_master_receive(dev, data, count, SEN5X_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "read of %u bytes from 0x%02x failed: %s", count, address,
                 esp_err_to_name(err));
        return -1;
    }
    return 0;
}

int8_t sensirion_i2c_hal_write(uint8_t address, const uint8_t *data, uint16_t count)
{
    i2c_master_dev_handle_t dev = hal_device(address);
    if (!dev) {
        return -1;
    }
    esp_err_t err = i2c_master_transmit(dev, data, count, SEN5X_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "write of %u bytes to 0x%02x failed: %s", count, address,
                 esp_err_to_name(err));
        return -1;
    }
    return 0;
}

void sensirion_i2c_hal_sleep_usec(uint32_t useconds)
{
    /* The driver's command delays run from tens of microseconds (busy-wait) to
     * the 100 ms+ of a device reset (yield, so the Zigbee stack keeps running).
     * FREERTOS_HZ is 1000, so a tick is 1 ms; round up and add one, since the
     * first tick of a vTaskDelay can elapse almost immediately. */
    if (useconds < 1000) {
        esp_rom_delay_us(useconds);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS((useconds + 999) / 1000) + 1);
}

int16_t sensirion_i2c_hal_select_bus(uint8_t bus_idx)
{
    /* Single bus, no multiplexer. */
    return bus_idx == 0 ? 0 : -1;
}
