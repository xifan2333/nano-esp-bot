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

#define BUTTON_MAX_PINS       8
#define BUTTON_QUEUE_DEPTH    32
#define BUTTON_TIMER_PERIOD_US 10000 // 10ms
#define BUTTON_DEFAULT_LONG_PRESS_MS 800
#define BUTTON_DEFAULT_DOUBLE_CLICK_MS 300

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
    int debounce_counter;
} chal_button_slot_t;

static SemaphoreHandle_t s_button_mutex = NULL;
static QueueHandle_t s_button_queue = NULL;
static esp_timer_handle_t s_button_timer = NULL;
static chal_button_slot_t s_buttons[BUTTON_MAX_PINS];
static int s_active_button_count = 0;

static int find_button_slot(int gpio_num)
{
    for (int i = 0; i < BUTTON_MAX_PINS; i++) {
        if (s_buttons[i].active && s_buttons[i].pin == gpio_num) {
            return i;
        }
    }
    return -1;
}

static int find_free_button_slot(void)
{
    for (int i = 0; i < BUTTON_MAX_PINS; i++) {
        if (!s_buttons[i].active) {
            return i;
        }
    }
    return -1;
}

static void post_event_locked(int pin, chal_button_event_type_t ev_type, uint32_t dur)
{
    if (!s_button_queue) {
        return;
    }

    if (uxQueueSpacesAvailable(s_button_queue) == 0) {
        chal_button_event_t drop;
        xQueueReceive(s_button_queue, &drop, 0);
    }

    chal_button_event_t ev = {
        .pin = pin,
        .event = ev_type,
        .press_duration_ms = dur,
    };
    xQueueSend(s_button_queue, &ev, 0);
}

static void button_timer_callback(void *arg)
{
    (void)arg;
    if (!s_button_mutex) {
        return;
    }

    if (xSemaphoreTake(s_button_mutex, 0) != pdTRUE) {
        return;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    for (int i = 0; i < BUTTON_MAX_PINS; i++) {
        chal_button_slot_t *slot = &s_buttons[i];
        if (!slot->active) {
            continue;
        }

        int raw_level = gpio_get_level((gpio_num_t)slot->pin);
        bool pressed = slot->config.active_low ? (raw_level == 0) : (raw_level == 1);

        switch (slot->state) {
        case BTN_STATE_IDLE:
            if (pressed) {
                slot->state = BTN_STATE_DEBOUNCE_PRESS;
                slot->debounce_counter = 1;
            }
            break;

        case BTN_STATE_DEBOUNCE_PRESS:
            if (pressed) {
                slot->debounce_counter++;
                if (slot->debounce_counter >= 2) {
                    slot->state = BTN_STATE_PRESSED;
                    slot->is_pressed_debounced = true;
                    slot->press_start_ms = now_ms;
                    post_event_locked(slot->pin, CHAL_BUTTON_EVENT_PRESS, 0);
                }
            } else {
                slot->state = BTN_STATE_IDLE;
            }
            break;

        case BTN_STATE_PRESSED:
            if (pressed) {
                uint32_t dur = now_ms - slot->press_start_ms;
                if (dur >= slot->config.long_press_ms) {
                    slot->state = BTN_STATE_WAIT_RELEASE;
                    post_event_locked(slot->pin, CHAL_BUTTON_EVENT_LONG_PRESS, dur);
                }
            } else {
                uint32_t dur = now_ms - slot->press_start_ms;
                slot->state = BTN_STATE_WAIT_DOUBLE;
                slot->is_pressed_debounced = false;
                slot->release_time_ms = now_ms;
                post_event_locked(slot->pin, CHAL_BUTTON_EVENT_RELEASE, dur);
            }
            break;

        case BTN_STATE_WAIT_RELEASE:
            if (!pressed) {
                slot->state = BTN_STATE_IDLE;
                slot->is_pressed_debounced = false;
                uint32_t dur = now_ms - slot->press_start_ms;
                post_event_locked(slot->pin, CHAL_BUTTON_EVENT_RELEASE, dur);
            }
            break;

        case BTN_STATE_WAIT_DOUBLE:
            if (pressed) {
                slot->state = BTN_STATE_WAIT_RELEASE;
                slot->is_pressed_debounced = true;
                slot->press_start_ms = now_ms;
                post_event_locked(slot->pin, CHAL_BUTTON_EVENT_PRESS, 0);
                post_event_locked(slot->pin, CHAL_BUTTON_EVENT_DOUBLE_CLICK, now_ms - slot->release_time_ms);
            } else {
                uint32_t gap = now_ms - slot->release_time_ms;
                if (gap >= slot->config.double_click_ms) {
                    slot->state = BTN_STATE_IDLE;
                    post_event_locked(slot->pin, CHAL_BUTTON_EVENT_CLICK, slot->release_time_ms - slot->press_start_ms);
                }
            }
            break;
        }
    }

    xSemaphoreGive(s_button_mutex);
}

esp_err_t chal_button_init_pin(int gpio_num, const chal_button_config_t *config)
{
    if (!ledger_is_pin_safe(gpio_num)) {
        ESP_LOGE(TAG, "GPIO %d is not safe for button input", gpio_num);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_button_mutex) {
        s_button_mutex = xSemaphoreCreateMutex();
        if (!s_button_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_button_mutex, portMAX_DELAY);

    if (!s_button_queue) {
        s_button_queue = xQueueCreate(BUTTON_QUEUE_DEPTH, sizeof(chal_button_event_t));
        if (!s_button_queue) {
            xSemaphoreGive(s_button_mutex);
            return ESP_ERR_NO_MEM;
        }
    }

    int slot_idx = find_button_slot(gpio_num);
    if (slot_idx < 0) {
        slot_idx = find_free_button_slot();
        if (slot_idx < 0) {
            xSemaphoreGive(s_button_mutex);
            ESP_LOGE(TAG, "No free button slots available (max %d)", BUTTON_MAX_PINS);
            return ESP_ERR_NO_MEM;
        }

        esp_err_t err = ledger_claim_pin(gpio_num, LEDGER_OWNER_SCRIPT, LEDGER_PIN_MODE_GPIO_IN, "button");
        if (err != ESP_OK) {
            xSemaphoreGive(s_button_mutex);
            ESP_LOGE(TAG, "Failed to claim GPIO %d for button: %s", gpio_num, esp_err_to_name(err));
            return err;
        }
    }

    chal_button_config_t final_cfg = {
        .active_low = true,
        .pull_up = true,
        .pull_down = false,
        .long_press_ms = BUTTON_DEFAULT_LONG_PRESS_MS,
        .double_click_ms = BUTTON_DEFAULT_DOUBLE_CLICK_MS,
    };
    if (config) {
        final_cfg = *config;
        if (final_cfg.long_press_ms == 0) final_cfg.long_press_ms = BUTTON_DEFAULT_LONG_PRESS_MS;
        if (final_cfg.double_click_ms == 0) final_cfg.double_click_ms = BUTTON_DEFAULT_DOUBLE_CLICK_MS;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = final_cfg.pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = final_cfg.pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_button_mutex);
        ESP_LOGE(TAG, "gpio_config failed for GPIO %d: %s", gpio_num, esp_err_to_name(err));
        return err;
    }

    s_buttons[slot_idx].active = true;
    s_buttons[slot_idx].pin = gpio_num;
    s_buttons[slot_idx].config = final_cfg;
    s_buttons[slot_idx].state = BTN_STATE_IDLE;
    s_buttons[slot_idx].is_pressed_debounced = false;
    s_buttons[slot_idx].press_start_ms = 0;
    s_buttons[slot_idx].release_time_ms = 0;
    s_buttons[slot_idx].debounce_counter = 0;
    s_active_button_count++;

    if (!s_button_timer) {
        esp_timer_create_args_t timer_args = {
            .callback = button_timer_callback,
            .arg = NULL,
            .name = "btn_timer",
        };
        err = esp_timer_create(&timer_args, &s_button_timer);
        if (err == ESP_OK) {
            esp_timer_start_periodic(s_button_timer, BUTTON_TIMER_PERIOD_US);
        } else {
            ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        }
    }

    xSemaphoreGive(s_button_mutex);
    ESP_LOGI(TAG, "Button initialized on GPIO %d (active_%s, pull_%s, long=%lu ms, dbl=%lu ms)",
             gpio_num,
             final_cfg.active_low ? "low" : "high",
             final_cfg.pull_up ? "up" : (final_cfg.pull_down ? "down" : "none"),
             (unsigned long)final_cfg.long_press_ms,
             (unsigned long)final_cfg.double_click_ms);
    return ESP_OK;
}

esp_err_t chal_button_deinit_pin(int gpio_num)
{
    if (!s_button_mutex) {
        return ESP_OK;
    }

    xSemaphoreTake(s_button_mutex, portMAX_DELAY);
    int slot_idx = find_button_slot(gpio_num);
    if (slot_idx < 0) {
        xSemaphoreGive(s_button_mutex);
        return ESP_OK;
    }

    s_buttons[slot_idx].active = false;
    s_active_button_count--;
    ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
    memset(&s_buttons[slot_idx], 0, sizeof(chal_button_slot_t));

    if (s_active_button_count <= 0) {
        s_active_button_count = 0;
        if (s_button_timer) {
            esp_timer_stop(s_button_timer);
            esp_timer_delete(s_button_timer);
            s_button_timer = NULL;
        }
    }

    xSemaphoreGive(s_button_mutex);
    ESP_LOGI(TAG, "Button deinitialized for GPIO %d", gpio_num);
    return ESP_OK;
}

bool chal_button_is_configured(int gpio_num)
{
    return find_button_slot(gpio_num) >= 0;
}

bool chal_button_is_pressed(int gpio_num)
{
    int slot_idx = find_button_slot(gpio_num);
    if (slot_idx >= 0) {
        return s_buttons[slot_idx].is_pressed_debounced;
    }
    return false;
}

uint32_t chal_button_get_queued_count(void)
{
    if (!s_button_queue) {
        return 0;
    }
    return (uint32_t)uxQueueMessagesWaiting(s_button_queue);
}

bool chal_button_recv(chal_button_event_t *out_event, uint32_t timeout_ms)
{
    if (!s_button_queue || !out_event) {
        return false;
    }

    TickType_t wait_ticks = timeout_ms ? pdMS_TO_TICKS(timeout_ms) : 0;
    return xQueueReceive(s_button_queue, out_event, wait_ticks) == pdPASS;
}

void chal_button_teardown(void)
{
    if (!s_button_mutex) {
        return;
    }

    if (xSemaphoreTake(s_button_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_button_timer) {
            esp_timer_stop(s_button_timer);
            esp_timer_delete(s_button_timer);
            s_button_timer = NULL;
        }

        for (int i = 0; i < BUTTON_MAX_PINS; i++) {
            if (s_buttons[i].active) {
                ledger_release_pin(s_buttons[i].pin, LEDGER_OWNER_SCRIPT);
                memset(&s_buttons[i], 0, sizeof(chal_button_slot_t));
            }
        }
        s_active_button_count = 0;

        if (s_button_queue) {
            vQueueDelete(s_button_queue);
            s_button_queue = NULL;
        }

        xSemaphoreGive(s_button_mutex);
    }
}

int luaopen_button(lua_State *L)
{
    lua_newtable(L);
    return 1;
}
