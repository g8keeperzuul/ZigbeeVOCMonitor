/*
 * LOCAL FILE (not from Waveshare).
 *
 * ESP-IDF implementation of the Waveshare DEV_Config HAL. Upstream ships this
 * for Raspberry Pi / Jetson only (bcm2835, wiringPi, lgpio, sysfs) and has no
 * ESP32 port for the 2.13" V4 panel at all, so this is written against IDF's
 * spi_master driver. Waveshare's own ESP-IDF 5.5 example for the C6 e-Paper
 * boards was the reference for the SPI setup.
 */

#include "DEV_Config.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epaper_hal";

/* The panel is the only device on SPI2, so the bus is claimed once at init and
 * held. That makes each of the ~4000 single-byte writes per frame a bare
 * polling transfer instead of an acquire/release round trip. */
#define EPD_SPI_HOST SPI2_HOST

int EPD_RST_PIN = EPD_RST_GPIO;
int EPD_DC_PIN = EPD_DC_GPIO;
int EPD_CS_PIN = EPD_CS_GPIO;
int EPD_BUSY_PIN = EPD_BUSY_GPIO;
int EPD_MOSI_PIN = EPD_MOSI_GPIO;
int EPD_SCLK_PIN = EPD_SCLK_GPIO;

static spi_device_handle_t s_spi;
static bool s_bus_held;

UBYTE DEV_Module_Init(void)
{
    if (s_spi) {
        return 0;
    }

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << EPD_RST_GPIO) | (1ULL << EPD_DC_GPIO) | (1ULL << EPD_CS_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));

    /* Pulled down so an unplugged panel reads "idle" rather than floating. The
     * alternative -- pull up, so a missing panel trips the BUSY timeout and says
     * so -- costs a 10 s stall on every operation during init. A blank screen is
     * the more obvious symptom anyway. */
    gpio_config_t busy_cfg = {
        .pin_bit_mask = 1ULL << EPD_BUSY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&busy_cfg));

    gpio_set_level(EPD_CS_GPIO, 1);
    gpio_set_level(EPD_DC_GPIO, 0);
    gpio_set_level(EPD_RST_GPIO, 1);

    /* Write-only panel: BUSY carries flow control, so there is no MISO. */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = EPD_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = EPD_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(EPD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi bus init failed: %s", esp_err_to_name(err));
        return 1;
    }

    /* CS stays a plain GPIO: the vendored driver raises and lowers it itself
     * around every command and data byte, and preserving those semantics is
     * worth more than letting the peripheral drive it. */
    spi_device_interface_config_t dev_cfg = {
        .mode = 0,
        .clock_speed_hz = EPD_SPI_HZ,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    err = spi_bus_add_device(EPD_SPI_HOST, &dev_cfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi add device failed: %s", esp_err_to_name(err));
        spi_bus_free(EPD_SPI_HOST);
        s_spi = NULL;
        return 1;
    }

    if (spi_device_acquire_bus(s_spi, portMAX_DELAY) == ESP_OK) {
        s_bus_held = true;
    }

    ESP_LOGI(TAG, "SPI2 ready: MOSI %d, SCLK %d, CS %d, DC %d, RST %d, BUSY %d @ %d kHz",
             EPD_MOSI_GPIO, EPD_SCLK_GPIO, EPD_CS_GPIO, EPD_DC_GPIO, EPD_RST_GPIO, EPD_BUSY_GPIO,
             EPD_SPI_HZ / 1000);
    return 0;
}

void DEV_Module_Exit(void)
{
    if (!s_spi) {
        return;
    }
    if (s_bus_held) {
        spi_device_release_bus(s_spi);
        s_bus_held = false;
    }
    spi_bus_remove_device(s_spi);
    spi_bus_free(EPD_SPI_HOST);
    s_spi = NULL;

    gpio_set_level(EPD_CS_GPIO, 0);
    gpio_set_level(EPD_DC_GPIO, 0);
    gpio_set_level(EPD_RST_GPIO, 0);
}

void DEV_Digital_Write(UWORD Pin, UBYTE Value)
{
    gpio_set_level(Pin, Value ? 1 : 0);
}

UBYTE DEV_Digital_Read(UWORD Pin)
{
    return (UBYTE)gpio_get_level(Pin);
}

void DEV_SPI_WriteByte(UBYTE Value)
{
    if (!s_spi) {
        return;
    }
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &Value,
    };
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "spi write failed: %s", esp_err_to_name(err));
    }
}

void DEV_SPI_Write_nByte(uint8_t *pData, uint32_t Len)
{
    if (!s_spi || Len == 0) {
        return;
    }
    spi_transaction_t t = {
        .length = Len * 8,
        .tx_buffer = pData,
    };
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "spi bulk write failed: %s", esp_err_to_name(err));
    }
}

void DEV_Delay_ms(UDOUBLE xms)
{
    /* The reset pulse asks for 2 ms and the driver leans on short delays being
     * at least as long as requested. vTaskDelay rounds toward zero and its first
     * tick can elapse immediately, so busy-wait the short ones. */
    if (xms == 0) {
        return;
    }
    if (xms < 10) {
        esp_rom_delay_us(xms * 1000);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(xms) + 1);
}
