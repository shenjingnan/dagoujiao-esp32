find_package(Python3 REQUIRED COMPONENTS Interpreter)

# 优先使用 ESP-IDF 的 Python 环境（带有本项目构建依赖，如 Pillow）
if(DEFINED ENV{IDF_PYTHON_ENV_PATH} AND EXISTS "$ENV{IDF_PYTHON_ENV_PATH}/bin/python")
    set(Python3_EXECUTABLE "$ENV{IDF_PYTHON_ENV_PATH}/bin/python" CACHE FILEPATH "Python interpreter" FORCE)
endif()

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

# 把大狗 PNG 预缩放并转成 LVGL 直读的 ARGB8888 C 数组，避免运行时 PNG 解码卡顿
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_ROOT}/tools/prepare_dog_images.py"
            --src-dir "${TOY_ROOT}"
            --out-c "${GENERATED_ASSET_DIR}/dog_images.c"
            --out-h "${GENERATED_ASSET_DIR}/dog_images.h"
    RESULT_VARIABLE DOG_IMAGE_RESULT
)
if(NOT DOG_IMAGE_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not convert the local 大狗 image assets")
endif()

idf_component_register(
    SRCS "bgm.c" "app_main.c" "bark_audio.c" "dog_ui.c" "fx_effects.c" "touch_async.c"
         "${GENERATED_ASSET_DIR}/bark_samples.c"
         "${GENERATED_ASSET_DIR}/dog_images.c"
    INCLUDE_DIRS "." "${GENERATED_ASSET_DIR}"
    REQUIRES waveshare__esp32_s3_touch_lcd_1_85B esp_codec_dev espressif__button espressif__esp_lcd_touch nvs_flash esp_timer esp_hw_support freertos lvgl
)
