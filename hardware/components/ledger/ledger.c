#include "ledger.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "soc/soc_caps.h"
#include "esp_log.h"

static const char *TAG = "ledger";

#define LEDGER_MAX_PINS SOC_GPIO_PIN_COUNT

typedef struct {
    ledger_owner_t owner;
    ledger_pin_mode_t mode;
    char tag[16];
} ledger_pin_entry_t;

static ledger_pin_entry_t s_pins[LEDGER_MAX_PINS];
static SemaphoreHandle_t s_mutex = NULL;
static bool s_initialized = false;

bool ledger_is_pin_safe(int gpio_num)
{
    if (gpio_num < 0 || gpio_num >= LEDGER_MAX_PINS) {
        return false;
    }
    // Reserved pins on ESP32-S3:
    // GPIO 19, 20: USB-Serial/JTAG & USB OTG D-/D+
    if (gpio_num == 19 || gpio_num == 20) {
        return false;
    }
    // GPIO 26..32: Octal SPI Flash & PSRAM
    if (gpio_num >= 26 && gpio_num <= 32) {
        return false;
    }
    return true;
}

esp_err_t ledger_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create ledger mutex");
        return ESP_ERR_NO_MEM;
    }
    memset(s_pins, 0, sizeof(s_pins));
    s_initialized = true;
    ESP_LOGI(TAG, "Hardware Resource Ledger initialized (%d pins)", LEDGER_MAX_PINS);
    return ESP_OK;
}

esp_err_t ledger_claim_pin(int gpio_num, ledger_owner_t owner, ledger_pin_mode_t mode, const char *tag)
{
    if (!s_initialized || !ledger_is_pin_safe(gpio_num) || owner == LEDGER_OWNER_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    if (s_pins[gpio_num].owner != LEDGER_OWNER_NONE) {
        ESP_LOGW(TAG, "Pin GPIO%d conflict: already claimed by owner %d (%s)",
                 gpio_num, s_pins[gpio_num].owner, s_pins[gpio_num].tag);
        ret = ESP_ERR_INVALID_STATE;
    } else {
        s_pins[gpio_num].owner = owner;
        s_pins[gpio_num].mode = mode;
        if (tag) {
            strncpy(s_pins[gpio_num].tag, tag, sizeof(s_pins[gpio_num].tag) - 1);
            s_pins[gpio_num].tag[sizeof(s_pins[gpio_num].tag) - 1] = '\0';
        } else {
            s_pins[gpio_num].tag[0] = '\0';
        }
        ESP_LOGD(TAG, "Claimed GPIO%d for owner %d mode %d (%s)",
                 gpio_num, owner, mode, s_pins[gpio_num].tag);
    }

    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t ledger_release_pin(int gpio_num, ledger_owner_t owner)
{
    if (!s_initialized || gpio_num < 0 || gpio_num >= LEDGER_MAX_PINS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    if (s_pins[gpio_num].owner == LEDGER_OWNER_NONE) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (s_pins[gpio_num].owner != owner) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        gpio_reset_pin(gpio_num);
        s_pins[gpio_num].owner = LEDGER_OWNER_NONE;
        s_pins[gpio_num].mode = LEDGER_PIN_MODE_FREE;
        s_pins[gpio_num].tag[0] = '\0';
    }

    xSemaphoreGive(s_mutex);
    return ret;
}

uint32_t ledger_teardown(ledger_owner_t owner)
{
    if (!s_initialized || owner == LEDGER_OWNER_NONE) {
        return 0;
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }

    uint32_t freed = 0;
    for (int i = 0; i < LEDGER_MAX_PINS; i++) {
        if (s_pins[i].owner == owner) {
            gpio_reset_pin(i);
            s_pins[i].owner = LEDGER_OWNER_NONE;
            s_pins[i].mode = LEDGER_PIN_MODE_FREE;
            s_pins[i].tag[0] = '\0';
            freed++;
        }
    }

    xSemaphoreGive(s_mutex);
    if (freed > 0) {
        ESP_LOGI(TAG, "Teardown owner %d: %lu pin(s) safely reset", (int)owner, (unsigned long)freed);
    }
    return freed;
}

uint32_t ledger_get_claimed_count(ledger_owner_t owner)
{
    if (!s_initialized) {
        return 0;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }
    uint32_t count = 0;
    for (int i = 0; i < LEDGER_MAX_PINS; i++) {
        if (owner == LEDGER_OWNER_NONE) {
            if (s_pins[i].owner != LEDGER_OWNER_NONE) count++;
        } else {
            if (s_pins[i].owner == owner) count++;
        }
    }
    xSemaphoreGive(s_mutex);
    return count;
}

bool ledger_is_pin_claimed(int gpio_num)
{
    if (!s_initialized || gpio_num < 0 || gpio_num >= LEDGER_MAX_PINS) {
        return false;
    }
    return s_pins[gpio_num].owner != LEDGER_OWNER_NONE;
}

ledger_owner_t ledger_get_pin_owner(int gpio_num)
{
    if (!s_initialized || gpio_num < 0 || gpio_num >= LEDGER_MAX_PINS) {
        return LEDGER_OWNER_NONE;
    }
    return s_pins[gpio_num].owner;
}
