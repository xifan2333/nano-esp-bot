#include "chal_i2c.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "ledger.h"
#include "lauxlib.h"

static const char *TAG = "chal_i2c";

#define I2C_MAX_DEVICES       8
#define I2C_DEFAULT_TIMEOUT_MS 100

typedef struct {
    uint8_t address;
    i2c_master_dev_handle_t handle;
    bool in_use;
} chal_i2c_dev_slot_t;

static SemaphoreHandle_t s_i2c_mutex = NULL;
static bool s_i2c_initialized = false;
static int s_sda_pin = -1;
static int s_scl_pin = -1;
static uint32_t s_speed_hz = 0;
static i2c_master_bus_handle_t s_bus_handle = NULL;
static chal_i2c_dev_slot_t s_devices[I2C_MAX_DEVICES];

static i2c_master_dev_handle_t get_or_create_device_locked(uint8_t device_addr)
{
    if (!s_bus_handle) {
        return NULL;
    }

    // Check if already created
    for (int i = 0; i < I2C_MAX_DEVICES; i++) {
        if (s_devices[i].in_use && s_devices[i].address == device_addr) {
            return s_devices[i].handle;
        }
    }

    // Find empty slot
    int slot = -1;
    for (int i = 0; i < I2C_MAX_DEVICES; i++) {
        if (!s_devices[i].in_use) {
            slot = i;
            break;
        }
    }

    // If full, evict slot 0
    if (slot < 0) {
        slot = 0;
        if (s_devices[0].handle) {
            i2c_master_bus_rm_device(s_devices[0].handle);
            s_devices[0].handle = NULL;
            s_devices[0].in_use = false;
        }
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = device_addr,
        .scl_speed_hz = s_speed_hz ? s_speed_hz : CHAL_I2C_DEFAULT_FREQ_HZ,
    };

    i2c_master_dev_handle_t handle = NULL;
    esp_err_t err = i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device 0x%02X: %s", device_addr, esp_err_to_name(err));
        return NULL;
    }

    s_devices[slot].address = device_addr;
    s_devices[slot].handle = handle;
    s_devices[slot].in_use = true;
    return handle;
}

esp_err_t chal_i2c_init(int sda_pin, int scl_pin, uint32_t speed_hz)
{
    if (!ledger_is_pin_safe(sda_pin)) {
        ESP_LOGE(TAG, "SDA GPIO %d is not safe for I2C", sda_pin);
        return ESP_ERR_INVALID_ARG;
    }
    if (!ledger_is_pin_safe(scl_pin)) {
        ESP_LOGE(TAG, "SCL GPIO %d is not safe for I2C", scl_pin);
        return ESP_ERR_INVALID_ARG;
    }
    if (sda_pin == scl_pin) {
        ESP_LOGE(TAG, "SDA and SCL cannot be the same pin (%d)", sda_pin);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_i2c_mutex) {
        s_i2c_mutex = xSemaphoreCreateMutex();
        if (!s_i2c_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    if (s_i2c_initialized) {
        xSemaphoreGive(s_i2c_mutex);
        ESP_LOGW(TAG, "I2C already initialized (SDA=%d, SCL=%d)", s_sda_pin, s_scl_pin);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ledger_claim_pin(sda_pin, LEDGER_OWNER_SCRIPT, LEDGER_PIN_MODE_I2C, "i2c_sda");
    if (err != ESP_OK) {
        xSemaphoreGive(s_i2c_mutex);
        ESP_LOGE(TAG, "Failed to claim SDA GPIO %d: %s", sda_pin, esp_err_to_name(err));
        return err;
    }

    err = ledger_claim_pin(scl_pin, LEDGER_OWNER_SCRIPT, LEDGER_PIN_MODE_I2C, "i2c_scl");
    if (err != ESP_OK) {
        ledger_release_pin(sda_pin, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_i2c_mutex);
        ESP_LOGE(TAG, "Failed to claim SCL GPIO %d: %s", scl_pin, esp_err_to_name(err));
        return err;
    }

    if (speed_hz == 0) {
        speed_hz = CHAL_I2C_DEFAULT_FREQ_HZ;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = 1,
        }
    };

    err = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (err != ESP_OK) {
        ledger_release_pin(scl_pin, LEDGER_OWNER_SCRIPT);
        ledger_release_pin(sda_pin, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_i2c_mutex);
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    for (int i = 0; i < I2C_MAX_DEVICES; i++) {
        s_devices[i].in_use = false;
        s_devices[i].handle = NULL;
    }

    s_i2c_initialized = true;
    s_sda_pin = sda_pin;
    s_scl_pin = scl_pin;
    s_speed_hz = speed_hz;
    xSemaphoreGive(s_i2c_mutex);
    ESP_LOGI(TAG, "I2C master bus initialized (SDA=%d, SCL=%d, freq=%lu Hz)",
             sda_pin, scl_pin, (unsigned long)speed_hz);
    return ESP_OK;
}

esp_err_t chal_i2c_deinit(void)
{
    if (!s_i2c_mutex) {
        return ESP_OK;
    }

    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    if (!s_i2c_initialized) {
        xSemaphoreGive(s_i2c_mutex);
        return ESP_OK;
    }

    for (int i = 0; i < I2C_MAX_DEVICES; i++) {
        if (s_devices[i].in_use && s_devices[i].handle) {
            i2c_master_bus_rm_device(s_devices[i].handle);
            s_devices[i].handle = NULL;
            s_devices[i].in_use = false;
        }
    }

    if (s_bus_handle) {
        i2c_del_master_bus(s_bus_handle);
        s_bus_handle = NULL;
    }

    if (s_sda_pin >= 0) {
        ledger_release_pin(s_sda_pin, LEDGER_OWNER_SCRIPT);
        s_sda_pin = -1;
    }
    if (s_scl_pin >= 0) {
        ledger_release_pin(s_scl_pin, LEDGER_OWNER_SCRIPT);
        s_scl_pin = -1;
    }

    s_speed_hz = 0;
    s_i2c_initialized = false;
    xSemaphoreGive(s_i2c_mutex);
    ESP_LOGI(TAG, "I2C master bus deinitialized");
    return ESP_OK;
}

bool chal_i2c_is_initialized(void)
{
    return s_i2c_initialized;
}

int chal_i2c_get_sda_pin(void)
{
    return s_sda_pin;
}

int chal_i2c_get_scl_pin(void)
{
    return s_scl_pin;
}

uint32_t chal_i2c_get_speed_hz(void)
{
    return s_speed_hz;
}

esp_err_t chal_i2c_probe(uint8_t device_addr, int timeout_ms)
{
    if (!s_i2c_initialized || !s_bus_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms <= 0) {
        timeout_ms = 50;
    }
    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    esp_err_t err = i2c_master_probe(s_bus_handle, device_addr, timeout_ms);
    xSemaphoreGive(s_i2c_mutex);
    return err;
}

esp_err_t chal_i2c_scan(uint8_t *out_addresses, size_t max_addresses, size_t *out_count)
{
    if (!s_i2c_initialized || !s_bus_handle || !out_count) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t found = 0;
    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        esp_err_t err = i2c_master_probe(s_bus_handle, addr, 20);
        if (err == ESP_OK) {
            if (out_addresses && found < max_addresses) {
                out_addresses[found] = addr;
            }
            found++;
        }
    }

    *out_count = found;
    xSemaphoreGive(s_i2c_mutex);
    return ESP_OK;
}

esp_err_t chal_i2c_write(uint8_t device_addr, const uint8_t *data, size_t len, int timeout_ms)
{
    if (!s_i2c_initialized || !s_bus_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms <= 0) {
        timeout_ms = I2C_DEFAULT_TIMEOUT_MS;
    }

    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    i2c_master_dev_handle_t dev = get_or_create_device_locked(device_addr);
    if (!dev) {
        xSemaphoreGive(s_i2c_mutex);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = i2c_master_transmit(dev, data, len, timeout_ms);
    xSemaphoreGive(s_i2c_mutex);
    return err;
}

esp_err_t chal_i2c_read(uint8_t device_addr, uint8_t *buffer, size_t len, int timeout_ms)
{
    if (!s_i2c_initialized || !s_bus_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (buffer == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms <= 0) {
        timeout_ms = I2C_DEFAULT_TIMEOUT_MS;
    }

    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    i2c_master_dev_handle_t dev = get_or_create_device_locked(device_addr);
    if (!dev) {
        xSemaphoreGive(s_i2c_mutex);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = i2c_master_receive(dev, buffer, len, timeout_ms);
    xSemaphoreGive(s_i2c_mutex);
    return err;
}

esp_err_t chal_i2c_write_reg(uint8_t device_addr, uint8_t reg_addr, const uint8_t *data, size_t len, int timeout_ms)
{
    if (!s_i2c_initialized || !s_bus_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms <= 0) {
        timeout_ms = I2C_DEFAULT_TIMEOUT_MS;
    }

    uint8_t stack_buf[65];
    uint8_t *tx_buf = stack_buf;
    if (len + 1 > sizeof(stack_buf)) {
        tx_buf = (uint8_t *)malloc(len + 1);
        if (!tx_buf) {
            return ESP_ERR_NO_MEM;
        }
    }

    tx_buf[0] = reg_addr;
    if (len > 0 && data != NULL) {
        memcpy(tx_buf + 1, data, len);
    }

    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    i2c_master_dev_handle_t dev = get_or_create_device_locked(device_addr);
    if (!dev) {
        xSemaphoreGive(s_i2c_mutex);
        if (tx_buf != stack_buf) free(tx_buf);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = i2c_master_transmit(dev, tx_buf, len + 1, timeout_ms);
    xSemaphoreGive(s_i2c_mutex);

    if (tx_buf != stack_buf) {
        free(tx_buf);
    }
    return err;
}

esp_err_t chal_i2c_read_reg(uint8_t device_addr, uint8_t reg_addr, uint8_t *buffer, size_t len, int timeout_ms)
{
    if (!s_i2c_initialized || !s_bus_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (buffer == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms <= 0) {
        timeout_ms = I2C_DEFAULT_TIMEOUT_MS;
    }

    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    i2c_master_dev_handle_t dev = get_or_create_device_locked(device_addr);
    if (!dev) {
        xSemaphoreGive(s_i2c_mutex);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = i2c_master_transmit_receive(dev, &reg_addr, 1, buffer, len, timeout_ms);
    xSemaphoreGive(s_i2c_mutex);
    return err;
}

void chal_i2c_teardown(void)
{
    chal_i2c_deinit();
}

int luaopen_i2c(lua_State *L)
{
    lua_newtable(L);
    return 1;
}
