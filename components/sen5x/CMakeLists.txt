# Sensirion SEN5x I2C driver, vendored from
# https://github.com/Sensirion/embedded-i2c-sen5x (BSD-3-Clause, see LICENSE).
# Everything here is upstream except sensirion_i2c_hal.c, which is our ESP-IDF
# port of the platform HAL.
idf_component_register(
    SRCS
        "sen5x_i2c.c"
        "sensirion_common.c"
        "sensirion_i2c.c"
        "sensirion_i2c_hal.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_common
)
