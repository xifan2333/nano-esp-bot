#include "chal_led_strip.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "ledger.h"
#include "lauxlib.h"

static const char *TAG = "chal_led_strip";

#define LED_STRIP_RESOLUTION_HZ 10000000

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} rmt_ws2812_encoder_t;

static size_t rmt_encode_ws2812(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                                const void *primary_data, size_t data_size,
                                rmt_encode_state_t *ret_state)
{
    rmt_ws2812_encoder_t *led_encoder = __containerof(encoder, rmt_ws2812_encoder_t, base);
    rmt_encoder_handle_t bytes_encoder = led_encoder->bytes_encoder;
    rmt_encoder_handle_t copy_encoder = led_encoder->copy_encoder;
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (led_encoder->state) {
    case 0:
        encoded_symbols += bytes_encoder->encode(bytes_encoder, channel, primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
        // fall-through
    case 1:
        encoded_symbols += copy_encoder->encode(copy_encoder, channel, &led_encoder->reset_code,
                                                sizeof(led_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = RMT_ENCODING_RESET;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
    }
out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t rmt_del_ws2812_encoder(rmt_encoder_t *encoder)
{
    rmt_ws2812_encoder_t *led_encoder = __containerof(encoder, rmt_ws2812_encoder_t, base);
    if (led_encoder->bytes_encoder) {
        rmt_del_encoder(led_encoder->bytes_encoder);
    }
    if (led_encoder->copy_encoder) {
        rmt_del_encoder(led_encoder->copy_encoder);
    }
    free(led_encoder);
    return ESP_OK;
}

static esp_err_t rmt_ws2812_encoder_reset(rmt_encoder_t *encoder)
{
    rmt_ws2812_encoder_t *led_encoder = __containerof(encoder, rmt_ws2812_encoder_t, base);
    rmt_encoder_reset(led_encoder->bytes_encoder);
    rmt_encoder_reset(led_encoder->copy_encoder);
    led_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t rmt_new_ws2812_encoder(uint32_t resolution, rmt_encoder_handle_t *ret_encoder)
{
    rmt_ws2812_encoder_t *led_encoder = rmt_alloc_encoder_mem(sizeof(rmt_ws2812_encoder_t));
    if (!led_encoder) {
        return ESP_ERR_NO_MEM;
    }
    memset(led_encoder, 0, sizeof(rmt_ws2812_encoder_t));
    led_encoder->base.encode = rmt_encode_ws2812;
    led_encoder->base.del = rmt_del_ws2812_encoder;
    led_encoder->base.reset = rmt_ws2812_encoder_reset;

    rmt_bytes_encoder_config_t bytes_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = (uint32_t)(0.3f * resolution / 1000000.0f),
            .level1 = 0,
            .duration1 = (uint32_t)(0.9f * resolution / 1000000.0f),
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = (uint32_t)(0.9f * resolution / 1000000.0f),
            .level1 = 0,
            .duration1 = (uint32_t)(0.3f * resolution / 1000000.0f),
        },
        .flags.msb_first = 1
    };

    esp_err_t err = rmt_new_bytes_encoder(&bytes_config, &led_encoder->bytes_encoder);
    if (err != ESP_OK) {
        free(led_encoder);
        return err;
    }

    rmt_copy_encoder_config_t copy_config = {};
    err = rmt_new_copy_encoder(&copy_config, &led_encoder->copy_encoder);
    if (err != ESP_OK) {
        rmt_del_encoder(led_encoder->bytes_encoder);
        free(led_encoder);
        return err;
    }

    uint32_t reset_ticks = resolution / 1000000 * 50 / 2;
    led_encoder->reset_code = (rmt_symbol_word_t){
        .level0 = 0,
        .duration0 = reset_ticks,
        .level1 = 0,
        .duration1 = reset_ticks,
    };

    *ret_encoder = &led_encoder->base;
    return ESP_OK;
}

static SemaphoreHandle_t s_led_mutex = NULL;
static bool s_inited = false;
static int s_gpio_num = 48;
static uint32_t s_num_leds = 1;
static rmt_channel_handle_t s_led_chan = NULL;
static rmt_encoder_handle_t s_led_encoder = NULL;
static uint8_t *s_pixel_buf = NULL;

esp_err_t chal_led_strip_init(int gpio_num, uint32_t num_leds)
{
    if (gpio_num < 0) {
        gpio_num = 48;
    }
    if (num_leds == 0) {
        num_leds = 1;
    }

    if (!s_led_mutex) {
        s_led_mutex = xSemaphoreCreateMutex();
        if (!s_led_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_led_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_inited) {
        xSemaphoreGive(s_led_mutex);
        return ESP_OK;
    }

    esp_err_t err = ledger_claim_pin(gpio_num, LEDGER_OWNER_SCRIPT, LEDGER_PIN_MODE_RMT, "led_strip");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to claim GPIO %d in ledger: %s", gpio_num, esp_err_to_name(err));
        xSemaphoreGive(s_led_mutex);
        return err;
    }

    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = gpio_num,
        .mem_block_symbols = 64,
        .resolution_hz = LED_STRIP_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };

    err = rmt_new_tx_channel(&tx_chan_config, &s_led_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_led_mutex);
        return err;
    }

    err = rmt_new_ws2812_encoder(LED_STRIP_RESOLUTION_HZ, &s_led_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_ws2812_encoder failed: %s", esp_err_to_name(err));
        rmt_del_channel(s_led_chan);
        s_led_chan = NULL;
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_led_mutex);
        return err;
    }

    err = rmt_enable(s_led_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        rmt_del_encoder(s_led_encoder);
        s_led_encoder = NULL;
        rmt_del_channel(s_led_chan);
        s_led_chan = NULL;
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_led_mutex);
        return err;
    }

    s_pixel_buf = calloc(num_leds, 3);
    if (!s_pixel_buf) {
        rmt_disable(s_led_chan);
        rmt_del_encoder(s_led_encoder);
        s_led_encoder = NULL;
        rmt_del_channel(s_led_chan);
        s_led_chan = NULL;
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_led_mutex);
        return ESP_ERR_NO_MEM;
    }

    s_gpio_num = gpio_num;
    s_num_leds = num_leds;
    s_inited = true;

    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    rmt_transmit(s_led_chan, s_led_encoder, s_pixel_buf, s_num_leds * 3, &tx_config);
    rmt_tx_wait_all_done(s_led_chan, portMAX_DELAY);

    xSemaphoreGive(s_led_mutex);
    return ESP_OK;
}

esp_err_t chal_led_strip_deinit(void)
{
    if (s_led_mutex && xSemaphoreTake(s_led_mutex, portMAX_DELAY) == pdTRUE) {
        if (!s_inited) {
            xSemaphoreGive(s_led_mutex);
            return ESP_OK;
        }

        if (s_pixel_buf && s_led_chan && s_led_encoder) {
            memset(s_pixel_buf, 0, s_num_leds * 3);
            rmt_transmit_config_t tx_config = { .loop_count = 0 };
            rmt_transmit(s_led_chan, s_led_encoder, s_pixel_buf, s_num_leds * 3, &tx_config);
            rmt_tx_wait_all_done(s_led_chan, pdMS_TO_TICKS(100));
        }

        if (s_led_chan) {
            rmt_disable(s_led_chan);
            rmt_del_channel(s_led_chan);
            s_led_chan = NULL;
        }

        if (s_led_encoder) {
            rmt_del_encoder(s_led_encoder);
            s_led_encoder = NULL;
        }

        if (s_pixel_buf) {
            free(s_pixel_buf);
            s_pixel_buf = NULL;
        }

        ledger_release_pin(s_gpio_num, LEDGER_OWNER_SCRIPT);
        s_inited = false;

        xSemaphoreGive(s_led_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

bool chal_led_strip_is_initialized(void)
{
    return s_inited;
}

int chal_led_strip_get_pin(void)
{
    return s_gpio_num;
}

uint32_t chal_led_strip_get_length(void)
{
    return s_num_leds;
}

esp_err_t chal_led_strip_set_pixel(uint32_t index, uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_inited || !s_pixel_buf) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= s_num_leds) {
        return ESP_ERR_INVALID_ARG;
    }

    s_pixel_buf[index * 3 + 0] = green;
    s_pixel_buf[index * 3 + 1] = red;
    s_pixel_buf[index * 3 + 2] = blue;
    return ESP_OK;
}

esp_err_t chal_led_strip_fill(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_inited || !s_pixel_buf) {
        return ESP_ERR_INVALID_STATE;
    }
    for (uint32_t i = 0; i < s_num_leds; i++) {
        s_pixel_buf[i * 3 + 0] = green;
        s_pixel_buf[i * 3 + 1] = red;
        s_pixel_buf[i * 3 + 2] = blue;
    }
    return ESP_OK;
}

esp_err_t chal_led_strip_refresh(void)
{
    if (!s_inited || !s_led_chan || !s_led_encoder || !s_pixel_buf) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_led_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    esp_err_t err = rmt_transmit(s_led_chan, s_led_encoder, s_pixel_buf, s_num_leds * 3, &tx_config);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_led_chan, portMAX_DELAY);
    }

    xSemaphoreGive(s_led_mutex);
    return err;
}

esp_err_t chal_led_strip_clear(void)
{
    if (!s_inited || !s_pixel_buf) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(s_pixel_buf, 0, s_num_leds * 3);
    return chal_led_strip_refresh();
}

void chal_led_strip_teardown(void)
{
    if (s_inited) {
        chal_led_strip_deinit();
    }
}

int luaopen_led_strip(lua_State *L)
{
    lua_newtable(L);
    return 1;
}
