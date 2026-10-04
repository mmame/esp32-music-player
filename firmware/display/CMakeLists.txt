# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.12)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Binary name follows the board (IDF_TARGET is already known here): esp32 = 2432S032, esp32s3 = 8048S050C
if(IDF_TARGET STREQUAL "esp32")
    project(ESP32-2432S032C)
else()
    project(ESP32-8048S050C)
endif()
