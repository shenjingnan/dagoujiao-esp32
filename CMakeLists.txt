cmake_minimum_required(VERSION 3.16)

# Reuse the locally supplied Waveshare BSP instead of duplicating its board
# definitions for the ST77916 display, CST816S touch and ES8311 audio codec.
set(WAVESHARE_EXAMPLE_ROOT "/Users/nemo/github/ESP32-S3-Touch-LCD-1.85B/Examples/ESP-IDF-V5.5.3")
set(WAVESHARE_BSP_DIR "${WAVESHARE_EXAMPLE_ROOT}/02_lvgl_demo/components/waveshare__esp32_s3_touch_lcd_1_85B")
set(WAVESHARE_IMU_DIR "${WAVESHARE_EXAMPLE_ROOT}/02_lvgl_demo/components/waveshare__qmi8658")
set(WAVESHARE_RTC_DIR "${WAVESHARE_EXAMPLE_ROOT}/03_mp3_player/components/waveshare__pcf85063a")

foreach(component_dir IN ITEMS ${WAVESHARE_BSP_DIR} ${WAVESHARE_IMU_DIR} ${WAVESHARE_RTC_DIR})
    if(NOT EXISTS "${component_dir}/CMakeLists.txt")
        message(FATAL_ERROR "Missing Waveshare board support component: ${component_dir}")
    endif()
endforeach()

set(EXTRA_COMPONENT_DIRS "${WAVESHARE_BSP_DIR}" "${WAVESHARE_IMU_DIR}" "${WAVESHARE_RTC_DIR}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dagoujiao_esp32)
