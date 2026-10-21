# ESP32-S3 platform layer for PrBoom (original work). wad_check.c is pure C and host tested.
# -Wpedantic omitted: ESP-IDF headers use #include_next and empty structs (see NOTES.md).
idf_component_register(SRCS wad_check.c wad_partition.c i_video_oled.c i_sprite_ids.c
                            i_system_esp32.c i_brain_input.c
                            i_sound_null.c
                       INCLUDE_DIRS include
                       REQUIRES esp_driver_gpio
                       PRIV_REQUIRES prboom oled_pipeline brain_link oled_driver esp_partition esp_timer
                                     esp_psram)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wconversion -Wshadow -Wdouble-promotion -DHAVE_CONFIG_H)
