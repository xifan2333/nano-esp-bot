#include "chal_ir.h"
#include "chal_i2c.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "ledger.h"
#include "lauxlib.h"

static const char *TAG = "chal_i2c";

#define I2C_MAX_DEVICES 8

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

esp_err_t chal_i2c_init(int sda_pin, int scl_pin, uint32_t speed_hz)
{
    (void)sda_pin;
    (void)scl_pin;
    (void)speed_hz;
    (void)s_bus_handle;
    (void)s_devices;
    (void)s_i2c_mutex;
    (void)TAG;
    return ESP_OK;
}

esp_err_t chal_i2c_deinit(void)
{
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

esp_err_t chal_i2c_scan(uint8_t *out_addresses, size_t max_addresses, size_t *out_count)
{
    (void)out_addresses;
    (void)max_addresses;
    if (out_count) *out_count = 0;
    return ESP_OK;
}

esp_err_t chal_i2c_probe(uint8_t device_addr, int timeout_ms)
{
    (void)device_addr;
    (void)timeout_ms;
    return ESP_OK;
}

esp_err_t chal_i2c_write(uint8_t device_addr, const uint8_t *data, size_t len, int timeout_ms)
{
    (void)device_addr;
    (void)data;
    (void)len;
    (void)timeout_ms;
    return ESP_OK;
}

esp_err_t chal_i2c_read(uint8_t device_addr, uint8_t *buffer, size_t len, int timeout_ms)
{
    (void)device_addr;
    (void)buffer;
    (void)len;
    (void)timeout_ms;
    return ESP_OK;
}

esp_err_t chal_i2c_write_reg(uint8_t device_addr, uint8_t reg_addr, const uint8_t *data, size_t len, int timeout_ms)
{
    (void)device_addr;
    (void)reg_addr;
    (void)data;
    (void)len;
    (void)timeout_ms;
    return ESP_OK;
}

esp_err_t chal_i2c_read_reg(uint8_t device_addr, uint8_t reg_addr, uint8_t *buffer, size_t len, int timeout_ms)
{
    (void)device_addr;
    (void)reg_addr;
    (void)buffer;
    (void)len;
    (void)timeout_ms;
    return ESP_OK;
}

void chal_i2c_teardown(void)
{
}

int luaopen_i2c(lua_State *L)
{
    lua_newtable(L);
    return 1;
}
