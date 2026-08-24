#pragma once

/*
 * Pure drawing for the e-Paper readout: given the strings to show, fill a
 * framebuffer. No ESP-IDF, no FreeRTOS, no panel I/O -- only GUI_Paint.
 *
 * Kept separate from display.c precisely so it can be compiled and run on a
 * host. See tools/epd-preview/, which renders this same file to a PNG so the
 * layout can be checked without flashing anything.
 */

#include "GUI_Paint.h"

/* Native panel geometry. Declared here rather than pulled from EPD_2in13_V4.h
 * so this file stays independent of the driver; display.c static-asserts that
 * the two agree. */
#define DISPLAY_PANEL_W 122
#define DISPLAY_PANEL_H 250

/* Drawing surface, landscape: the native dimensions swapped. */
#define DISPLAY_CANVAS_W DISPLAY_PANEL_H
#define DISPLAY_CANVAS_H DISPLAY_PANEL_W

#define DISPLAY_FRAMEBUFFER_BYTES \
    ((DISPLAY_PANEL_W % 8 == 0 ? DISPLAY_PANEL_W / 8 : DISPLAY_PANEL_W / 8 + 1) * DISPLAY_PANEL_H)

/* Everything that appears on screen, already formatted. display.c compares two
 * of these to decide whether a refresh would change any pixels. */
typedef struct {
    char voc[8]; /* the bare number, drawn double-size */
    char pm25[24];
    char temp[16];
    char humidity[16];
} display_fields_t;

/* Draws the full screen into framebuffer. Caller must have set it up with
 * Paint_NewImage(fb, DISPLAY_PANEL_W, DISPLAY_PANEL_H, ROTATE_90, WHITE). */
void display_layout_render(UBYTE *framebuffer, const display_fields_t *fields);
