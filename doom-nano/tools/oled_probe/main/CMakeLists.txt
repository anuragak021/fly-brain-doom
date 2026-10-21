idf_component_register(SRCS "oled_probe.c"
                       PRIV_REQUIRES esp_driver_i2c esp_psram spi_flash)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion)
