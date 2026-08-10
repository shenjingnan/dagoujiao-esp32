#include "bark_audio.h"
#include "dog_ui.h"

#include "bsp/esp-bsp.h"
#include "button_gpio.h"
#include "iot_button.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "dagoujiao";

static void boot_button_event(void *arg, void *data)
{
    (void)arg;
    (void)data;
    bark_audio_toggle_mute();
    ESP_LOGI(TAG, "global mute: %s", bark_audio_is_muted() ? "on" : "off");
}

static void init_boot_button(void)
{
    button_config_t cfg = {0};
    button_gpio_config_t gpio_cfg = {
        .gpio_num = BSP_BUTTONS_IO_0,
        .active_level = 0,
    };
    button_handle_t button = NULL;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&cfg, &gpio_cfg, &button));
    ESP_ERROR_CHECK(iot_button_register_cb(button, BUTTON_SINGLE_CLICK, NULL, boot_button_event, NULL));
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    bark_audio_init();
    dog_ui_init();
    init_boot_button();
    ESP_LOGI(TAG, "大狗叫 is ready; tap the screen to start the beat");
}
