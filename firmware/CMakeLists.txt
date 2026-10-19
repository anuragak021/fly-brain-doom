cmake_minimum_required(VERSION 3.16)
project(brain_firmware C ASM)

# Standalone brain application for the STM32H753ZI, linked at 0x08000000.
#   cmake -S firmware -B build/fw -DCMAKE_TOOLCHAIN_FILE=firmware/cmake/arm-none-eabi.cmake
#   cmake --build build/fw
option(BRAIN_CPU_400MHZ "Run the core at 400 MHz from PLL1 (default: 64 MHz HSI)" OFF)
option(BRAIN_ESP_USART6 "ESP32 link on USART6 (PG14 TX, PG9 RX) instead of USART1 (PB6 TX, PB7 RX)" OFF)
option(BRAIN_CACHES "Enable the L1 instruction and data caches" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(CMSIS ${ROOT}/../third_party/cmsis)

set(OUR_SOURCES
    ${ROOT}/src/main.c
    ${ROOT}/src/brain_server.c
    ${ROOT}/src/brain_protocol.c
    ${ROOT}/src/brain_link.c
    ${ROOT}/src/rate_model.c
    ${ROOT}/src/plasticity.c
    ${ROOT}/generated/model_weights.c
    ${ROOT}/port/board.c
    ${ROOT}/port/uart_port.c
    ${ROOT}/port/crt_stubs.c)
set(VENDOR_SOURCES
    ${CMSIS}/device/startup_stm32h753xx.s
    ${CMSIS}/device/system_stm32h7xx.c)

add_executable(brain ${OUR_SOURCES} ${VENDOR_SOURCES})
target_include_directories(brain PRIVATE
    ${ROOT}/src ${ROOT}/include ${ROOT}/generated ${ROOT}/port)
# SYSTEM: warnings inside the vendor CMSIS headers are not ours to fix.
target_include_directories(brain SYSTEM PRIVATE ${CMSIS}/core ${CMSIS}/device)
target_compile_definitions(brain PRIVATE STM32H753xx)

# Strict warnings, as errors, for our own code only. Vendor files are built without -Werror.
set_source_files_properties(${OUR_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wdouble-promotion;-Werror")

if(BRAIN_CPU_400MHZ)
    target_compile_definitions(brain PRIVATE BRAIN_CPU_400MHZ)
endif()
if(BRAIN_ESP_USART6)
    target_compile_definitions(brain PRIVATE BRAIN_ESP_USART6)
endif()
if(BRAIN_CACHES)
    target_compile_definitions(brain PRIVATE BRAIN_CACHES)
endif()

target_compile_options(brain PRIVATE ${MCU_FLAGS} -O2 -g3 -ffunction-sections -fdata-sections)
target_link_options(brain PRIVATE ${MCU_FLAGS}
    -T${ROOT}/linker/brain.ld -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/brain.map
    -specs=nano.specs -specs=nosys.specs -nostartfiles)
target_link_libraries(brain PRIVATE m)

add_custom_command(TARGET brain POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:brain> ${CMAKE_BINARY_DIR}/brain.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:brain>)
