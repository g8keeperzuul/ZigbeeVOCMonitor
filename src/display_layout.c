/*
 * Screen layout for the e-Paper readout.
 *
 * Deliberately free of ESP-IDF dependencies so tools/epd-preview/ can compile
 * this exact file on a host and render it to a PNG. Anything that touches the
 * panel, the task or the refresh policy belongs in display.c instead.
 */

#include "display_layout.h"

#include <string.h>

/*
 * Draw text at twice the font's size. Waveshare's largest font is 24 px, which
 * is under 5 mm on this panel -- fine to read at arm's length, not across a
 * room. Doubling Font24 gives a 34x48 headline.
 *
 * The bitmap walk mirrors Paint_DrawChar() exactly, including advancing the
 * pointer past the row padding when the glyph width is not a multiple of 8.
 */
static void draw_string_2x(UWORD x, UWORD y, const char *text, sFONT *font)
{
    const UWORD bytes_per_row = font->Width / 8 + (font->Width % 8 ? 1 : 0);
    UWORD cursor = x;

    for (const char *c = text; *c != '\0'; c++) {
        const unsigned char *ptr = &font->table[(*c - ' ') * font->Height * bytes_per_row];

        for (UWORD row = 0; row < font->Height; row++) {
            for (UWORD col = 0; col < font->Width; col++) {
                if (*ptr & (0x80 >> (col % 8))) {
                    UWORD px = cursor + col * 2;
                    UWORD py = y + row * 2;
                    Paint_SetPixel(px, py, BLACK);
                    Paint_SetPixel(px + 1, py, BLACK);
                    Paint_SetPixel(px, py + 1, BLACK);
                    Paint_SetPixel(px + 1, py + 1, BLACK);
                }
                if (col % 8 == 7) {
                    ptr++;
                }
            }
            if (font->Width % 8 != 0) {
                ptr++;
            }
        }
        cursor += font->Width * 2;
    }
}

/*
 * Layout, following the supplied template. 250 x 122, landscape.
 *
 *   0            125                 187    250
 *   +-------------+---------------------------+ 0
 *   | VOC Index   |          PM 2.5           |
 *   | (white on   |                           |
 *   |  black)  28 |        xx.x ug/m3         |
 *   +-------------+                           |
 *   |             |                           |
 *   |     aaa     +-------------+-------------+ 60
 *   |             |   yy.y C    |  zz.z %RH   |
 *   +-------------+-------------+-------------+ 122
 */
#define SPLIT_X    125 /* main vertical divider */
#define HEADER_Y   28  /* bottom of the black VOC Index box */
#define RIGHT_MID_Y 60 /* PM2.5 block above, T/RH below */
#define RH_SPLIT_X 187 /* divides temperature from humidity */

/* Centre text horizontally in [x0, x1). Falls back to left-aligned rather than
 * negative if the string is wider than the cell. */
static void draw_centered(UWORD x0, UWORD x1, UWORD y, const char *text, sFONT *font)
{
    UWORD width = (UWORD)strlen(text) * font->Width;
    UWORD cell = x1 - x0;
    UWORD x = (width >= cell) ? x0 : x0 + (cell - width) / 2;
    Paint_DrawString_EN(x, y, text, font, BLACK, WHITE);
}

static void draw_centered_2x(UWORD x0, UWORD x1, UWORD y, const char *text, sFONT *font)
{
    UWORD width = (UWORD)strlen(text) * font->Width * 2;
    UWORD cell = x1 - x0;
    UWORD x = (width >= cell) ? x0 : x0 + (cell - width) / 2;
    draw_string_2x(x, y, text, font);
}

void display_layout_render(UBYTE *framebuffer, const display_fields_t *s)
{
    Paint_SelectImage(framebuffer);
    Paint_Clear(WHITE);

    /* Frame and dividers. */
    Paint_DrawRectangle(0, 0, DISPLAY_CANVAS_W - 1, DISPLAY_CANVAS_H - 1, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    Paint_DrawLine(SPLIT_X, 0, SPLIT_X, DISPLAY_CANVAS_H - 1, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    Paint_DrawLine(SPLIT_X, RIGHT_MID_Y, DISPLAY_CANVAS_W - 1, RIGHT_MID_Y, BLACK, DOT_PIXEL_1X1,
                   LINE_STYLE_SOLID);
    Paint_DrawLine(RH_SPLIT_X, RIGHT_MID_Y, RH_SPLIT_X, DISPLAY_CANVAS_H - 1, BLACK, DOT_PIXEL_1X1,
                   LINE_STYLE_SOLID);

    /* Reversed-out header. Fill first, then white glyphs over it. */
    Paint_DrawRectangle(0, 0, SPLIT_X, HEADER_Y, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    UWORD label_w = (UWORD)strlen("VOC Index") * Font16.Width;
    Paint_DrawString_EN((SPLIT_X - label_w) / 2, (HEADER_Y - Font16.Height) / 2, "VOC Index",
                        &Font16, WHITE, BLACK);

    /* The headline, vertically centred in what is left of the left column. */
    draw_centered_2x(0, SPLIT_X, HEADER_Y + (DISPLAY_CANVAS_H - HEADER_Y - Font24.Height * 2) / 2, s->voc,
                     &Font24);

    draw_centered(SPLIT_X, DISPLAY_CANVAS_W, 6, "PM 2.5", &Font16);
    draw_centered(SPLIT_X, DISPLAY_CANVAS_W, 34, s->pm25, &Font16);

    UWORD row_y = RIGHT_MID_Y + (DISPLAY_CANVAS_H - RIGHT_MID_Y - Font12.Height) / 2;
    draw_centered(SPLIT_X, RH_SPLIT_X, row_y, s->temp, &Font12);
    draw_centered(RH_SPLIT_X, DISPLAY_CANVAS_W, row_y, s->humidity, &Font12);
}
