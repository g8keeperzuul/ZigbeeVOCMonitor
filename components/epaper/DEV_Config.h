/*
 * LOCAL FILE (not from Waveshare).
 *
 * Replaces the upstream lib/Config/DEV_Config.h, which pulls in bcm2835 /
 * wiringPi / lgpio and POSIX headers that do not exist on ESP-IDF. The contract
 * below is deliberately identical to upstream's so the vendored driver and
 * GUI_Paint compile against it unmodified.
 *
 * Pin assignment for this board. GPIO 6 and 7 -- the SPI2 IOMUX pins, and the
 * pins Waveshare's wiring guide suggests -- are already the SEN55's I2C bus, so
 * the display runs through the GPIO matrix instead. That is fine: the matrix is
 * good for tens of MHz and this panel is clocked at 2.
 */
#ifndef _DEV_CONFIG_H_
#define _DEV_CONFIG_H_

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Debug.h"

/* Upstream type names, used throughout the vendored sources. */
#define UBYTE   uint8_t
#define UWORD   uint16_t
#define UDOUBLE uint32_t

#ifndef EPD_MOSI_GPIO
#define EPD_MOSI_GPIO 23
#endif
#ifndef EPD_SCLK_GPIO
#define EPD_SCLK_GPIO 22
#endif
#ifndef EPD_CS_GPIO
#define EPD_CS_GPIO 10
#endif
#ifndef EPD_DC_GPIO
#define EPD_DC_GPIO 19
#endif
#ifndef EPD_RST_GPIO
#define EPD_RST_GPIO 20
#endif
#ifndef EPD_BUSY_GPIO
#define EPD_BUSY_GPIO 21
#endif

/* The SSD1680 accepts 20 MHz, but Waveshare's own drivers run 2 MHz and this
 * panel is on flying leads. Raise only after it is proven stable. */
#ifndef EPD_SPI_HZ
#define EPD_SPI_HZ (2 * 1000 * 1000)
#endif

/* Upstream refers to the pins through these globals, not through the macros. */
extern int EPD_RST_PIN;
extern int EPD_DC_PIN;
extern int EPD_CS_PIN;
extern int EPD_BUSY_PIN;
extern int EPD_MOSI_PIN;
extern int EPD_SCLK_PIN;

UBYTE DEV_Module_Init(void); /* 0 on success, matching upstream */
void DEV_Module_Exit(void);

void DEV_Digital_Write(UWORD Pin, UBYTE Value);
UBYTE DEV_Digital_Read(UWORD Pin);

void DEV_SPI_WriteByte(UBYTE Value);
void DEV_SPI_Write_nByte(uint8_t *pData, uint32_t Len);

void DEV_Delay_ms(UDOUBLE xms);

#endif
