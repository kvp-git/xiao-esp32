#include "esp_littlefs.h"
#include "esp_log.h"
#include "display_ssd1351.h"
#include "player_task.h"
#include "usb_cli.h"

static const char *TAG = "MAIN";

void app_main(void) {
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&conf));

    ESP_ERROR_CHECK(ssd1351_init());

    player_start();
    usb_cli_start();

    ESP_LOGI(TAG, "System initialized.");
}
