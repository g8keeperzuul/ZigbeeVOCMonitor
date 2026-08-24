/*
 * Renders src/display_layout.c on the host so the e-Paper layout can be checked
 * without flashing. Compiles the real layout file -- not a copy of it -- so what
 * comes out is what the device will draw.
 *
 *   make preview                       the standard set of cases
 *   ./epd-preview 137 "8.1 ug/m3" "21.9 C" "61.0 %RH" out.pbm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display_layout.h"

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: %s VOC PM25 TEMP HUMIDITY out.pbm\n", argv[0]);
        return 2;
    }

    UBYTE *framebuffer = malloc(DISPLAY_FRAMEBUFFER_BYTES);
    if (!framebuffer) {
        return 1;
    }

    /* Exactly the call display.c makes. */
    Paint_NewImage(framebuffer, DISPLAY_PANEL_W, DISPLAY_PANEL_H, ROTATE_90, WHITE);

    display_fields_t fields = {0};
    snprintf(fields.voc, sizeof(fields.voc), "%s", argv[1]);
    snprintf(fields.pm25, sizeof(fields.pm25), "%s", argv[2]);
    snprintf(fields.temp, sizeof(fields.temp), "%s", argv[3]);
    snprintf(fields.humidity, sizeof(fields.humidity), "%s", argv[4]);

    display_layout_render(framebuffer, &fields);

    FILE *f = fopen(argv[5], "wb");
    if (!f) {
        perror("fopen");
        return 1;
    }
    /* Raw PBM, native panel orientation. 1 means black, the inverse of the
     * panel's own convention, hence the complement. */
    fprintf(f, "P4\n%d %d\n", DISPLAY_PANEL_W, DISPLAY_PANEL_H);
    for (int i = 0; i < DISPLAY_FRAMEBUFFER_BYTES; i++) {
        fputc((unsigned char)~framebuffer[i], f);
    }
    fclose(f);

    /* Report the widest string against its cell, so an overflow shows up here
     * rather than as clipped pixels nobody notices. */
    printf("%s: voc=%zu pm25=%zu temp=%zu rh=%zu chars\n", argv[5], strlen(fields.voc),
           strlen(fields.pm25), strlen(fields.temp), strlen(fields.humidity));
    return 0;
}
