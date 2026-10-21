# Phase 1 hardware demo: laptop streams 128x64 gray frames, ESP32-S3 dithers, packs, drives OLED.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../components)
# Only build what main needs (keeps the PrBoom component out of this app).
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(oled_stream)
