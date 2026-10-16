#include "chal_pwm.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "ledger.h"
#include "lauxlib.h"

static const char *TAG = "chal_pwm";

#define PWM_MAX_CHANNELS 8
#define PWM_MAX_TIMERS   4

typedef struct {
    bool active;
    int gpio_num;
    ledc_channel_t channel;
    ledc_timer_t timer;
    uint32_t freq_hz;
    ledc_timer_bit_t resolution;
    uint32_t duty;
    uint32_t max_duty;
} pwm_channel_info_t;

typedef struct {
    bool in_use;
    uint32_t freq_hz;
    ledc_timer_bit_t resolution;
    int ref_count;
} pwm_timer_info_t;

static SemaphoreHandle_t s_pwm_mutex = NULL;
static pwm_channel_info_t s_channels[PWM_MAX_CHANNELS];
static pwm_timer_info_t s_timers[PWM_MAX_TIMERS];

static int find_channel_by_gpio(int gpio_num)
{
    for (int i = 0; i < PWM_MAX_CHANNELS; i++) {
        if (s_channels[i].active && s_channels[i].gpio_num == gpio_num) {
            return i;
        }
    }
    return -1;
}

static int alloc_channel(void)
{
    for (int i = 0; i < PWM_MAX_CHANNELS; i++) {
        if (!s_channels[i].active) {
            return i;
        }
    }
    return -1;
}

static int alloc_or_share_timer(uint32_t freq_hz, ledc_timer_bit_t resolution)
{
    for (int i = 0; i < PWM_MAX_TIMERS; i++) {
        if (s_timers[i].in_use && s_timers[i].freq_hz == freq_hz && s_timers[i].resolution == resolution) {
            s_timers[i].ref_count++;
            return i;
        }
    }
    for (int i = 0; i < PWM_MAX_TIMERS; i++) {
        if (!s_timers[i].in_use) {
            ledc_timer_config_t timer_conf = {
                .speed_mode = LEDC_LOW_SPEED_MODE,
                .duty_resolution = resolution,
                .timer_num = (ledc_timer_t)i,
                .freq_hz = freq_hz,
                .clk_cfg = LEDC_AUTO_CLK,
                .deconfigure = false,
            };
            esp_err_t err = ledc_timer_config(&timer_conf);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "ledc_timer_config failed for timer %d: %s", i, esp_err_to_name(err));
                return -1;
            }
            s_timers[i].in_use = true;
            s_timers[i].freq_hz = freq_hz;
            s_timers[i].resolution = resolution;
            s_timers[i].ref_count = 1;
            return i;
        }
    }
    return -1;
}

static void release_timer(ledc_timer_t timer_idx)
{
    int idx = (int)timer_idx;
    if (idx >= 0 && idx < PWM_MAX_TIMERS && s_timers[idx].in_use) {
        s_timers[idx].ref_count--;
        if (s_timers[idx].ref_count <= 0) {
            s_timers[idx].in_use = false;
            s_timers[idx].ref_count = 0;
            ledc_timer_pause(LEDC_LOW_SPEED_MODE, timer_idx);
        }
    }
}

esp_err_t chal_pwm_attach(int gpio_num, uint32_t freq_hz, uint8_t duty_resolution)
{
    if (gpio_num < 0 || gpio_num > 48 || freq_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (duty_resolution < 1 || duty_resolution > 14) {
        duty_resolution = 14;
    }

    if (!s_pwm_mutex) {
        s_pwm_mutex = xSemaphoreCreateMutex();
        if (!s_pwm_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_pwm_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (find_channel_by_gpio(gpio_num) >= 0) {
        xSemaphoreGive(s_pwm_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ledger_claim_pin(gpio_num, LEDGER_OWNER_SCRIPT, LEDGER_PIN_MODE_PWM, "pwm");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to claim GPIO %d in ledger: %s", gpio_num, esp_err_to_name(err));
        xSemaphoreGive(s_pwm_mutex);
        return err;
    }

    int ch_idx = alloc_channel();
    if (ch_idx < 0) {
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_pwm_mutex);
        return ESP_ERR_NO_MEM;
    }

    int timer_idx = alloc_or_share_timer(freq_hz, (ledc_timer_bit_t)duty_resolution);
    if (timer_idx < 0) {
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_pwm_mutex);
        return ESP_ERR_NO_MEM;
    }

    ledc_channel_config_t ch_conf = {
        .gpio_num = gpio_num,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)ch_idx,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = (ledc_timer_t)timer_idx,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags = {
            .output_invert = 0,
        },
    };

    err = ledc_channel_config(&ch_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(err));
        release_timer((ledc_timer_t)timer_idx);
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_pwm_mutex);
        return err;
    }

    s_channels[ch_idx].active = true;
    s_channels[ch_idx].gpio_num = gpio_num;
    s_channels[ch_idx].channel = (ledc_channel_t)ch_idx;
    s_channels[ch_idx].timer = (ledc_timer_t)timer_idx;
    s_channels[ch_idx].freq_hz = freq_hz;
    s_channels[ch_idx].resolution = (ledc_timer_bit_t)duty_resolution;
    s_channels[ch_idx].duty = 0;
    s_channels[ch_idx].max_duty = (1UL << duty_resolution) - 1;

    xSemaphoreGive(s_pwm_mutex);
    return ESP_OK;
}

esp_err_t chal_pwm_write_duty(int gpio_num, uint32_t duty)
{
    if (s_pwm_mutex && xSemaphoreTake(s_pwm_mutex, portMAX_DELAY) == pdTRUE) {
        int idx = find_channel_by_gpio(gpio_num);
        if (idx < 0) {
            xSemaphoreGive(s_pwm_mutex);
            return ESP_ERR_NOT_FOUND;
        }

        if (duty > s_channels[idx].max_duty) {
            duty = s_channels[idx].max_duty;
        }

        esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, s_channels[idx].channel, duty);
        if (err == ESP_OK) {
            err = ledc_update_duty(LEDC_LOW_SPEED_MODE, s_channels[idx].channel);
            if (err == ESP_OK) {
                s_channels[idx].duty = duty;
            }
        }
        xSemaphoreGive(s_pwm_mutex);
        return err;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t chal_pwm_write_ratio(int gpio_num, float ratio)
{
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;

    if (s_pwm_mutex && xSemaphoreTake(s_pwm_mutex, portMAX_DELAY) == pdTRUE) {
        int idx = find_channel_by_gpio(gpio_num);
        if (idx < 0) {
            xSemaphoreGive(s_pwm_mutex);
            return ESP_ERR_NOT_FOUND;
        }

        uint32_t duty = (uint32_t)(ratio * (float)s_channels[idx].max_duty + 0.5f);
        esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, s_channels[idx].channel, duty);
        if (err == ESP_OK) {
            err = ledc_update_duty(LEDC_LOW_SPEED_MODE, s_channels[idx].channel);
            if (err == ESP_OK) {
                s_channels[idx].duty = duty;
            }
        }
        xSemaphoreGive(s_pwm_mutex);
        return err;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t chal_pwm_set_servo_angle(int gpio_num, float angle_deg)
{
    if (angle_deg < 0.0f) angle_deg = 0.0f;
    if (angle_deg > 180.0f) angle_deg = 180.0f;

    if (!chal_pwm_is_attached(gpio_num)) {
        esp_err_t err = chal_pwm_attach(gpio_num, 50, 14);
        if (err != ESP_OK) {
            return err;
        }
    }

    float pulse_us = 500.0f + (angle_deg / 180.0f) * 2000.0f;
    float duty_ratio = pulse_us / 20000.0f;

    return chal_pwm_write_ratio(gpio_num, duty_ratio);
}

esp_err_t chal_pwm_detach(int gpio_num)
{
    if (s_pwm_mutex && xSemaphoreTake(s_pwm_mutex, portMAX_DELAY) == pdTRUE) {
        int idx = find_channel_by_gpio(gpio_num);
        if (idx < 0) {
            xSemaphoreGive(s_pwm_mutex);
            return ESP_ERR_NOT_FOUND;
        }

        ledc_stop(LEDC_LOW_SPEED_MODE, s_channels[idx].channel, 0);
        release_timer(s_channels[idx].timer);
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);

        s_channels[idx].active = false;
        s_channels[idx].gpio_num = -1;
        s_channels[idx].duty = 0;

        xSemaphoreGive(s_pwm_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

bool chal_pwm_is_attached(int gpio_num)
{
    return find_channel_by_gpio(gpio_num) >= 0;
}

uint32_t chal_pwm_get_duty(int gpio_num)
{
    int idx = find_channel_by_gpio(gpio_num);
    if (idx >= 0) {
        return s_channels[idx].duty;
    }
    return 0;
}

void chal_pwm_teardown(void)
{
    if (!s_pwm_mutex) return;

    if (xSemaphoreTake(s_pwm_mutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < PWM_MAX_CHANNELS; i++) {
            if (s_channels[i].active) {
                ledc_stop(LEDC_LOW_SPEED_MODE, s_channels[i].channel, 0);
                release_timer(s_channels[i].timer);
                ledger_release_pin(s_channels[i].gpio_num, LEDGER_OWNER_SCRIPT);
                s_channels[i].active = false;
                s_channels[i].gpio_num = -1;
                s_channels[i].duty = 0;
            }
        }
        xSemaphoreGive(s_pwm_mutex);
    }
}

int luaopen_pwm(lua_State *L)
{
    lua_newtable(L);
    return 1;
}
