# doom-esp32: PrBoom on the ESP32-S3, rendered to a 128x64 SSD1306 through components/oled_pipeline.
cmake_minimum_required(VERSION 3.16)
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(doom_esp32)
