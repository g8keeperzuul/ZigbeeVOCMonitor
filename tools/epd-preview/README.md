# e-Paper layout preview

Renders the e-Paper screen on your machine, so the layout can be checked without
flashing the board.

```sh
make preview      # typical.png, warmup.png, extremes.png
make clean
```

Or one case at a time:

```sh
make
./epd-preview '137' '8.1 ug/m3' '21.9 C' '61.0 %RH' out.pbm
python3 pbm2png.py out.pbm out.png
```

## What it actually tests

It compiles [`src/display_layout.c`](../../src/display_layout.c) — the real
file the firmware builds, not a copy — against the same vendored `GUI_Paint.c`
and font tables in [`components/epaper/`](../../components/epaper). The only
substitutions are in `stub/`, which replaces the two headers that would drag in
ESP-IDF. So the pixels here are the pixels the panel gets.

That is the reason `display_layout.c` is kept free of ESP-IDF dependencies. Put
anything touching the panel, the task or the refresh policy in `display.c`
instead, or this stops building.

## Two things worth knowing

**The stubs are force-included, not shadowed.** The vendored sources write
`#include "DEV_Config.h"`, and a quoted include resolves against the including
file's own directory before any `-I` path — so `-Istub` on its own cannot win.
The Makefile passes `-include stub/DEV_Config.h`, which defines the include
guards up front and leaves the firmware versions expanding to nothing.

**Orientation is this tool's assumption, not a measurement.** The framebuffer is
native portrait (122x250); `pbm2png.py` rotates it to the 250x122 landscape view
to match what `ROTATE_90` should produce. If the physical panel ever comes out
rotated or mirrored relative to these PNGs, the rotation in `to_landscape()` is
what needs revisiting, not the layout.

`make preview` renders a typical reading, the warm-up placeholder, and the widest
value each field can produce (VOC 500, PM2.5 at four digits, sub-zero
temperature, 100 %RH). Check the extremes case after any layout change — cells
are tight and `Paint_SetPixel` clips silently rather than complaining.
