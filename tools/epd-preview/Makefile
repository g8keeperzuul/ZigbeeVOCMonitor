# Host preview for the e-Paper layout. See README.md.
ROOT := ../..
EPD  := $(ROOT)/components/epaper
SRC  := $(ROOT)/src

# The vendored sources #include "DEV_Config.h" in quotes, which resolves against
# their own directory before any -I path -- so -Istub alone cannot shadow the
# firmware's ESP-IDF-flavoured headers. Force-including the stubs first defines
# their include guards (_DEV_CONFIG_H_, _DEBUG_H_), so the real ones expand to
# nothing when they are reached. -Wno-type-limits is for vendored GUI_Paint.c.
CFLAGS  := -std=c11 -O1 -Wall -Wextra -Wno-type-limits -Wno-unused-parameter \
           -include stub/DEV_Config.h -Istub -I$(EPD) -I$(SRC)
SOURCES := harness.c $(SRC)/display_layout.c \
           $(EPD)/GUI_Paint.c $(EPD)/font12.c $(EPD)/font16.c $(EPD)/font24.c

.PHONY: preview clean

epd-preview: $(SOURCES)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) -lm

# The cases worth looking at every time the layout changes: a typical reading,
# the warm-up placeholder, and the widest values each field can produce.
preview: epd-preview
	./epd-preview '137' '8.1 ug/m3' '21.9 C' '61.0 %RH' typical.pbm
	./epd-preview '--' '-- ug/m3' '-- C' '-- %RH' warmup.pbm
	./epd-preview '500' '1000 ug/m3' '-10.5 C' '100 %RH' extremes.pbm
	@for f in typical warmup extremes; do python3 pbm2png.py $$f.pbm $$f.png; done

clean:
	rm -f epd-preview *.pbm *.png
