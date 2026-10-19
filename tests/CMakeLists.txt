cmake_minimum_required(VERSION 3.16)
project(drosophila_host_tests C)

# Host build of the pure-logic rate model. The STM32 project comes in Phase D.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../firmware)

add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wdouble-promotion -Werror)

add_executable(test_rate_model
    c/test_rate_model.c
    ${FW}/src/rate_model.c
    ${FW}/generated/model_weights.c)
target_include_directories(test_rate_model PRIVATE ${FW}/src ${FW}/include ${FW}/generated)
target_link_libraries(test_rate_model PRIVATE m)

add_executable(test_brain_protocol
    c/test_brain_protocol.c
    ${FW}/src/brain_protocol.c)
target_include_directories(test_brain_protocol PRIVATE ${FW}/include)
target_link_libraries(test_brain_protocol PRIVATE m)

add_executable(test_brain_server
    c/test_brain_server.c
    ${FW}/src/brain_server.c
    ${FW}/src/brain_protocol.c
    ${FW}/src/rate_model.c
    ${FW}/generated/model_weights.c)
target_include_directories(test_brain_server PRIVATE ${FW}/src ${FW}/include ${FW}/generated)
target_link_libraries(test_brain_server PRIVATE m)

add_executable(test_brain_link
    c/test_brain_link.c
    ${FW}/src/brain_link.c
    ${FW}/src/brain_server.c
    ${FW}/src/brain_protocol.c
    ${FW}/src/rate_model.c
    ${FW}/generated/model_weights.c)
target_include_directories(test_brain_link PRIVATE ${FW}/src ${FW}/include ${FW}/generated)
target_link_libraries(test_brain_link PRIVATE m)

enable_testing()
add_test(NAME rate_model_vs_python
         COMMAND test_rate_model ${CMAKE_CURRENT_SOURCE_DIR}/golden/rate_golden.csv)
add_test(NAME brain_protocol_vectors
         COMMAND test_brain_protocol ${CMAKE_CURRENT_SOURCE_DIR}/golden/protocol_vectors.txt)
add_test(NAME brain_server COMMAND test_brain_server)
add_test(NAME brain_link COMMAND test_brain_link)
