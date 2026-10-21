idf_component_register(SRCS "oled_stream_main.c"
# -Wpedantic omitted: ESP-IDF headers use #include_next and empty structs (see NOTES.md).
                       PRIV_REQUIRES oled_pipeline demo_stream oled_driver
                                     esp_driver_usb_serial_jtag esp_driver_uart esp_timer)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wconversion -Wshadow -Wdouble-promotion)
