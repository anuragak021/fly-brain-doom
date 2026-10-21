# Host build: pipeline library (static for tests, shared for the Python preview/stream tools)
# and the unit tests. Usage:
#   cmake -S host -B host/build && cmake --build host/build && ctest --test-dir host/build
cmake_minimum_required(VERSION 3.16)
project(doom_esp32_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OP_SANITIZE "Build with address and undefined behaviour sanitizers" ON)

set(OP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components/oled_pipeline)
set(OP_SOURCES
    ${OP_DIR}/op_gray.c ${OP_DIR}/op_scale.c ${OP_DIR}/op_dither.c ${OP_DIR}/op_outline.c ${OP_DIR}/op_mask.c ${OP_DIR}/op_nano.c
    ${OP_DIR}/op_hud.c ${OP_DIR}/op_pack.c)
set(OP_WARNINGS
    -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wdouble-promotion)

add_library(oled_pipeline STATIC ${OP_SOURCES})
target_include_directories(oled_pipeline PUBLIC ${OP_DIR}/include)
target_compile_options(oled_pipeline PRIVATE ${OP_WARNINGS})
target_link_libraries(oled_pipeline PUBLIC m)

# Same sources, position independent, loaded by host/preview and host/stream through ctypes so
# the PNG preview is produced by exactly the code the ESP32 runs.
add_library(oled_pipeline_shared SHARED ${OP_SOURCES})
target_include_directories(oled_pipeline_shared PUBLIC ${OP_DIR}/include)
target_compile_options(oled_pipeline_shared PRIVATE ${OP_WARNINGS})
target_link_libraries(oled_pipeline_shared PUBLIC m)
set_target_properties(oled_pipeline_shared PROPERTIES OUTPUT_NAME oled_pipeline)

set(DS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components/demo_stream)
add_library(demo_stream STATIC ${DS_DIR}/demo_stream.c)
target_include_directories(demo_stream PUBLIC ${DS_DIR}/include)
target_compile_options(demo_stream PRIVATE ${OP_WARNINGS})

set(DP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components/doom_platform)
add_library(wad_check STATIC ${DP_DIR}/wad_check.c)
target_include_directories(wad_check PUBLIC ${DP_DIR}/include)
target_compile_options(wad_check PRIVATE ${OP_WARNINGS})

# Brain link pure logic. The protocol header comes from the drosophila project (it owns it).
set(BL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components/brain_link)
set(DROSOPHILA_FW ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware)
add_library(brain_link_pure STATIC ${BL_DIR}/bl_features.c ${BL_DIR}/bl_policy.c ${BL_DIR}/bl_map.c
            ${DROSOPHILA_FW}/src/brain_protocol.c)
target_include_directories(brain_link_pure PUBLIC ${BL_DIR}/include ${DROSOPHILA_FW}/include)
target_compile_options(brain_link_pure PRIVATE ${OP_WARNINGS})
target_link_libraries(brain_link_pure PUBLIC oled_pipeline m)

if(OP_SANITIZE)
    foreach(lib oled_pipeline demo_stream wad_check brain_link_pure)
        target_compile_options(${lib} PRIVATE -fsanitize=address,undefined -g)
        target_link_options(${lib} PUBLIC -fsanitize=address,undefined)
    endforeach()
endif()

enable_testing()
foreach(t test_gray test_scale test_dither test_pack test_demo_stream test_outline test_mask test_nano test_hud
    test_wad_check)
    add_executable(${t} tests/${t}.c)
    target_include_directories(${t} PRIVATE tests)
    target_compile_options(${t} PRIVATE ${OP_WARNINGS})
    target_link_libraries(${t} PRIVATE oled_pipeline demo_stream wad_check)
    if(OP_SANITIZE)
        target_compile_options(${t} PRIVATE -fsanitize=address,undefined -g)
    endif()
    add_test(NAME ${t} COMMAND ${t})
endforeach()

foreach(t test_bl_features test_bl_policy test_bl_map)
    add_executable(${t} tests/${t}.c)
    target_include_directories(${t} PRIVATE tests)
    target_compile_options(${t} PRIVATE ${OP_WARNINGS})
    target_link_libraries(${t} PRIVATE brain_link_pure)
    if(OP_SANITIZE)
        target_compile_options(${t} PRIVATE -fsanitize=address,undefined -g)
    endif()
endforeach()
add_test(NAME test_bl_features
         COMMAND test_bl_features ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/bl_feature_vectors.txt)
add_test(NAME test_bl_policy COMMAND test_bl_policy)
add_test(NAME test_bl_map COMMAND test_bl_map)
