#include "chal_adc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "ledger.h"
#include "lauxlib.h"

static const char *TAG = "chal_adc";

#define ADC_MAX_PINS 10

typedef struct {
    bool active;
    int gpio_num;
    adc_unit_t unit;
    adc_channel_t channel;
    adc_atten_t atten;
    adc_cali_handle_t cali_handle;
    bool calibrated;
} chal_adc_slot_t;

static SemaphoreHandle_t s_adc_mutex = NULL;
static adc_oneshot_unit_handle_t s_unit1_handle = NULL;
static adc_oneshot_unit_handle_t s_unit2_handle = NULL;
static chal_adc_slot_t s_slots[ADC_MAX_PINS];

esp_err_t chal_adc_init_pin(int gpio_num, adc_atten_t atten)
{
    (void)gpio_num;
    (void)atten;
    (void)s_adc_mutex;
    (void)s_unit1_handle;
    (void)s_unit2_handle;
    (void)s_slots;
    (void)TAG;
    return ESP_OK;
}

esp_err_t chal_adc_deinit_pin(int gpio_num)
{
    (void)gpio_num;
    return ESP_OK;
}

esp_err_t chal_adc_read_raw(int gpio_num, int *out_raw)
{
    (void)gpio_num;
    if (out_raw) *out_raw = 0;
    return ESP_OK;
}

esp_err_t chal_adc_read_mv(int gpio_num, int *out_mv)
{
    (void)gpio_num;
    if (out_mv) *out_mv = 0;
    return ESP_OK;
}

esp_err_t chal_adc_read(int gpio_num, int *out_raw, int *out_mv)
{
    (void)gpio_num;
    if (out_raw) *out_raw = 0;
    if (out_mv) *out_mv = 0;
    return ESP_OK;
}

bool chal_adc_is_pin_configured(int gpio_num)
{
    (void)gpio_num;
    return false;
}

void chal_adc_teardown(void)
{
}

int luaopen_adc(lua_State *L)
{
    lua_newtable(L);
    return 1;
}
