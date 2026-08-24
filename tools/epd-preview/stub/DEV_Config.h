/*
 * Host stub for the e-Paper HAL header.
 *
 * GUI_Paint.h includes DEV_Config.h, and the real one pulls in driver/spi_master.h
 * and the IDF logger. Only the integer typedefs are actually needed to draw into
 * a framebuffer, so this supplies those and nothing else. Keeping stub/ first on
 * the include path shadows the firmware version.
 */
#ifndef _DEV_CONFIG_H_
#define _DEV_CONFIG_H_

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Debug.h"

#define UBYTE   uint8_t
#define UWORD   uint16_t
#define UDOUBLE uint32_t

#endif
