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

static const char *TAG = "nano-core";

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

    /* 1. Initialize Hardware Resource Ledger */
    ESP_ERROR_CHECK(ledger_init());

    /* 2. Initialize JSON-RPC 2.0 Engine & USB Listener */
    ESP_ERROR_CHECK(rpc_init());

    /* 3. Initialize Lua 5.4 VM Sandbox, Link Break Callback & Register RPC */
    ESP_ERROR_CHECK(sandbox_init());
    rpc_set_break_callback((rpc_break_callback_t)sandbox_break);
    ESP_ERROR_CHECK(sandbox_register_rpc());

    ESP_LOGI(TAG, "Micro-Supervisor is active. Control plane & Lua sandbox ready.");
}
