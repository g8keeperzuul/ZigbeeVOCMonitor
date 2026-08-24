# Sources are listed explicitly rather than globbed: a GLOB of "src/*.*" would
# also pick up idf_component.yml and hand it to the compiler.
idf_component_register(
    SRCS
        "main.c"
        "led.c"
        "sensor.c"
        "zb_sensor.c"
        "display.c"
        "display_layout.c"
    INCLUDE_DIRS "."
    REQUIRES sen5x epaper
)
