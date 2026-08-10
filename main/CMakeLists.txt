find_package(Python3 REQUIRED COMPONENTS Interpreter)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(TOY_ROOT "${PROJECT_ROOT}/tmp/大狗大狗叫叫叫_files")
set(GENERATED_ASSET_DIR "${CMAKE_BINARY_DIR}/generated_assets")
file(MAKE_DIRECTORY "${GENERATED_ASSET_DIR}")

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_ROOT}/tools/prepare_assets.py"
            --audio-data "${TOY_ROOT}/audio-data.js"
            --out-c "${GENERATED_ASSET_DIR}/bark_samples.c"
            --out-h "${GENERATED_ASSET_DIR}/bark_samples.h"
    RESULT_VARIABLE ASSET_CONVERSION_RESULT
)
if(NOT ASSET_CONVERSION_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not convert the local 大狗叫 audio assets")
endif()

idf_component_register(
    SRCS "bgm.c" "app_main.c" "bark_audio.c" "dog_ui.c" "${GENERATED_ASSET_DIR}/bark_samples.c"
    INCLUDE_DIRS "." "${GENERATED_ASSET_DIR}"
    REQUIRES waveshare__esp32_s3_touch_lcd_1_85B esp_codec_dev espressif__button nvs_flash esp_timer freertos lvgl
    EMBED_FILES
        "${TOY_ROOT}/dagou_close_mouth.png"
        "${TOY_ROOT}/dagou_open_mouth.png"
)
