#include "chal_button.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "ledger.h"
#include "lauxlib.h"

static const char *TAG = "chal_button";

#define BUTTON_MAX_PINS 8

typedef enum {
    BTN_STATE_IDLE = 0,
    BTN_STATE_DEBOUNCE_PRESS,
    BTN_STATE_PRESSED,
    BTN_STATE_WAIT_RELEASE,
    BTN_STATE_WAIT_DOUBLE,
} chal_btn_internal_state_t;

typedef struct {
    bool active;
    int pin;
    chal_button_config_t config;
    chal_btn_internal_state_t state;
    bool is_pressed_debounced;
    uint32_t press_start_ms;
    uint32_t release_time_ms;
    uint32_t press_duration_ms;
    int debounce_counter;
} chal_button_slot_t;

static SemaphoreHandle_t s_button_mutex = NULL;
static QueueHandle_t s_button_queue = NULL;
static esp_timer_handle_t s_button_timer = NULL;
static chal_button_slot_t s_buttons[BUTTON_MAX_PINS];

esp_err_t chal_button_init_pin(int gpio_num, const chal_button_config_t *config)
{
    (void)gpio_num;
    (void)config;
    (void)s_button_mutex;
    (void)s_button_queue;
    (void)s_button_timer;
    (void)s_buttons;
    (void)TAG;
    return ESP_OK;
}

esp_err_t chal_button_deinit_pin(int gpio_num)
{
    (void)gpio_num;
    return ESP_OK;
}

bool chal_button_is_configured(int gpio_num)
{
    (void)gpio_num;
    return false;
}

bool chal_button_is_pressed(int gpio_num)
{
    (void)gpio_num;
    return false;
}

uint32_t chal_button_get_queued_count(void)
{
    return 0;
}

bool chal_button_recv(chal_button_event_t *out_event, uint32_t timeout_ms)
{
    (void)out_event;
    (void)timeout_ms;
    return false;
}

void chal_button_teardown(void)
{
}

int luaopen_button(lua_State *L)
{
    lua_newtable(L);
    return 1;
}
