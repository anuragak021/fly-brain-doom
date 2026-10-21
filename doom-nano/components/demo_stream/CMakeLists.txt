# Phase 1 demo stream protocol (pure C). Built by ESP-IDF, and by host/CMakeLists.txt for tests.
if(ESP_PLATFORM)
    idf_component_register(SRCS demo_stream.c INCLUDE_DIRS include)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wdouble-promotion)
endif()
