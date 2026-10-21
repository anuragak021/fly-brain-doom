# Brain link: feature extraction and policy are pure C (host tested); the client is ESP-IDF.
# The protocol implementation is compiled from the drosophila project, which owns it:
# <repo>/firmware/include/brain_protocol.h and src/brain_protocol.c. It is not copied.
set(DROSOPHILA_FW ${CMAKE_CURRENT_LIST_DIR}/../../../firmware)

if(ESP_PLATFORM)
    idf_component_register(SRCS bl_features.c bl_policy.c bl_map.c bl_client.c
                                ${DROSOPHILA_FW}/src/brain_protocol.c
                           INCLUDE_DIRS include ${DROSOPHILA_FW}/include
                           REQUIRES esp_driver_gpio
                           PRIV_REQUIRES oled_pipeline esp_driver_uart esp_timer)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wall -Wextra -Werror -Wconversion -Wshadow -Wdouble-promotion)
endif()
