# Pure C display pipeline. Built by ESP-IDF as a component, and by host/CMakeLists.txt for tests.
set(OP_SOURCES op_gray.c op_scale.c op_dither.c op_outline.c op_mask.c op_nano.c op_hud.c op_pack.c)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${OP_SOURCES} INCLUDE_DIRS include)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wdouble-promotion)
endif()
