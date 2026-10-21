idf_component_register(SRCS "app_main.c"
                       PRIV_REQUIRES doom_platform oled_driver brain_link)
# -Wpedantic omitted: ESP-IDF headers use #include_next and empty structs (see NOTES.md).
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wconversion -Wshadow -Wdouble-promotion)
