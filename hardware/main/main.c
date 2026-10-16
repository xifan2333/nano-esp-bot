#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "ledger.h"
#include "rpc.h"
#include "sandbox.h"
#include "storage.h"
#include "chal_gpio.h"
#include "chal_delay.h"
#include "chal_wifi.h"
#include "chal_ble_hid.h"
#include "chal_ble.h"

static const char *TAG = "nano-core";

#define INIT_LUA_PATH "/storage/init.lua"

static void on_supervisor_break(void)
{
    sandbox_break();
    chal_wifi_teardown();
    chal_ble_hid_teardown();
    chal_ble_teardown();
}

static void init_script_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "Starting runtime task for %s", INIT_LUA_PATH);

    sandbox_result_t res;
    esp_err_t err = sandbox_eval_file(INIT_LUA_PATH, &res);
    if (err == ESP_OK) {
        if (res.success) {
            ESP_LOGI(TAG, "%s completed successfully (result: %s, insns: %lu, mem: %zu B)",
                     INIT_LUA_PATH, res.output, (unsigned long)res.insn_count, res.mem_used);
        } else {
            ESP_LOGW(TAG, "%s terminated with error: %s", INIT_LUA_PATH, res.output);
        }
    } else {
        ESP_LOGE(TAG, "Failed to load %s (%s)", INIT_LUA_PATH, esp_err_to_name(err));
    }

    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "  nano-esp-bot micro-supervisor     ");
    ESP_LOGI(TAG, "====================================");

    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "Target: %s (rev v%d.%d, %d cores)",
             CONFIG_IDF_TARGET,
             chip_info.revision / 100,
             chip_info.revision % 100,
             chip_info.cores);

    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "Flash size: %" PRIu32 " MB", flash_size / (1024 * 1024));
    }
    ESP_LOGI(TAG, "Free heap: %" PRIu32 " bytes", esp_get_free_heap_size());

    ESP_ERROR_CHECK(ledger_init());
    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(sandbox_init());
    rpc_set_break_callback(on_supervisor_break);
    ESP_ERROR_CHECK(sandbox_register_module("gpio", luaopen_gpio));
    ESP_ERROR_CHECK(sandbox_register_module("delay", luaopen_delay));
    ESP_ERROR_CHECK(sandbox_register_module("wifi", luaopen_wifi));
    ESP_ERROR_CHECK(sandbox_register_module("ble_hid", luaopen_ble_hid));
    ESP_ERROR_CHECK(sandbox_register_module("ble", luaopen_ble));
    ESP_ERROR_CHECK(rpc_init());

    if (storage_file_exists("init.lua")) {
        ESP_LOGI(TAG, "Found startup script %s. Launching runtime...", INIT_LUA_PATH);
        xTaskCreatePinnedToCore(init_script_task, "lua_runner", 8192, NULL, 5, NULL, 1);
    } else {
        ESP_LOGI(TAG, "No %s found. Standing by in idle mode.", INIT_LUA_PATH);
    }

    ESP_LOGI(TAG, "Micro-Supervisor is active. Control plane & runtime online.");
}
