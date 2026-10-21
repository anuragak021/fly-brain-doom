# SSD1306 I2C driver (ESP-IDF only).
# -Wpedantic omitted: ESP-IDF headers use #include_next and empty structs (see NOTES.md).
idf_component_register(SRCS oled_ssd1306.c
                       INCLUDE_DIRS include
                       REQUIRES esp_driver_gpio
                       PRIV_REQUIRES esp_driver_i2c)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wconversion -Wshadow -Wdouble-promotion)
