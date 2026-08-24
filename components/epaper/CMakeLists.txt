# Waveshare 2.13" e-Paper (V4) driver.
#
# Vendored from https://github.com/waveshareteam/e-Paper, from
# RaspberryPi_JetsonNano/c/lib/. Each upstream file carries its own MIT-style
# permission notice in its header comment; the repository has no separate
# LICENSE file.
#
# NOTE: the ESP32 example in that repo under E-paper_Separate_Program/
# 2in13_e-Paper_G/ is for the four-colour 2.13" G panel -- different controller,
# 2 bits per pixel, no partial refresh. It will not drive a V4.
#
# Local files, not from upstream:  DEV_Config.c, DEV_Config.h, Debug.h
# Patched upstream files (each patch marked "LOCAL PATCH" in place):
#   EPD_2in13_V4.c  bounded the otherwise infinite BUSY wait
#   EPD_2in13_V4.h  dropped EPD_2in13_V4_Init_GUI(), declared but never defined
#   GUI_Paint.h     fonts.h include path, flat layout instead of ../Fonts/
idf_component_register(
    SRCS
        "EPD_2in13_V4.c"
        "GUI_Paint.c"
        "font24.c"
        "font16.c"
        "font12.c"
        "DEV_Config.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_spi esp_driver_gpio esp_common
)

# Upstream GUI_Paint.c compares unsigned expressions against 0. Harmless, but it
# is their code -- scope the suppression to this component rather than patching
# vendored source or relaxing warnings project-wide.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-type-limits)
