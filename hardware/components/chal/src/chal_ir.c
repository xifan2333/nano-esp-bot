#include "chal_ir.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_encoder.h"
#include "ledger.h"
#include "lauxlib.h"

static const char *TAG = "chal_ir";

#define IR_RESOLUTION_HZ            1000000 // 1MHz, 1 tick = 1us
#define IR_NEC_CARRIER_HZ           38000
#define IR_RC5_CARRIER_HZ           36000
#define IR_RX_QUEUE_DEPTH           16
#define IR_RX_MEM_BLOCK_SYMBOLS     64

#define NEC_LEADING_CODE_DURATION_0 9000
#define NEC_LEADING_CODE_DURATION_1 4500
#define NEC_PAYLOAD_ZERO_DURATION_0 560
#define NEC_PAYLOAD_ZERO_DURATION_1 560
#define NEC_PAYLOAD_ONE_DURATION_0  560
#define NEC_PAYLOAD_ONE_DURATION_1  1690
#define NEC_REPEAT_CODE_DURATION_0  9000
#define NEC_REPEAT_CODE_DURATION_1  2250
#define NEC_DECODE_MARGIN           300

#define RC5_HALF_BIT_US             889
#define RC5_DECODE_MARGIN           250

typedef struct {
    uint16_t address;
    uint16_t command;
} ir_nec_scan_code_t;

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *copy_encoder;
    rmt_encoder_t *bytes_encoder;
    rmt_symbol_word_t nec_leading_symbol;
    rmt_symbol_word_t nec_ending_symbol;
    rmt_symbol_word_t nec_repeat_symbol;
    int state;
    bool is_repeat;
} rmt_ir_nec_encoder_t;

static size_t rmt_encode_ir_nec(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                                const void *primary_data, size_t data_size,
                                rmt_encode_state_t *ret_state)
{
    rmt_ir_nec_encoder_t *nec_encoder = __containerof(encoder, rmt_ir_nec_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;
    ir_nec_scan_code_t *scan_code = (ir_nec_scan_code_t *)primary_data;
    rmt_encoder_handle_t copy_encoder = nec_encoder->copy_encoder;
    rmt_encoder_handle_t bytes_encoder = nec_encoder->bytes_encoder;

    if (nec_encoder->is_repeat) {
        switch (nec_encoder->state) {
        case 0:
            encoded_symbols += copy_encoder->encode(copy_encoder, channel, &nec_encoder->nec_repeat_symbol,
                                                    sizeof(rmt_symbol_word_t), &session_state);
            if (session_state & RMT_ENCODING_COMPLETE) {
                nec_encoder->state = 1;
            }
            if (session_state & RMT_ENCODING_MEM_FULL) {
                state |= RMT_ENCODING_MEM_FULL;
                goto out;
            }
            // fall-through
        case 1:
            encoded_symbols += copy_encoder->encode(copy_encoder, channel, &nec_encoder->nec_ending_symbol,
                                                    sizeof(rmt_symbol_word_t), &session_state);
            if (session_state & RMT_ENCODING_COMPLETE) {
                nec_encoder->state = RMT_ENCODING_RESET;
                state |= RMT_ENCODING_COMPLETE;
            }
            if (session_state & RMT_ENCODING_MEM_FULL) {
                state |= RMT_ENCODING_MEM_FULL;
                goto out;
            }
        }
    } else {
        switch (nec_encoder->state) {
        case 0:
            encoded_symbols += copy_encoder->encode(copy_encoder, channel, &nec_encoder->nec_leading_symbol,
                                                    sizeof(rmt_symbol_word_t), &session_state);
            if (session_state & RMT_ENCODING_COMPLETE) {
                nec_encoder->state = 1;
            }
            if (session_state & RMT_ENCODING_MEM_FULL) {
                state |= RMT_ENCODING_MEM_FULL;
                goto out;
            }
            // fall-through
        case 1:
            encoded_symbols += bytes_encoder->encode(bytes_encoder, channel, &scan_code->address,
                                                    sizeof(uint16_t), &session_state);
            if (session_state & RMT_ENCODING_COMPLETE) {
                nec_encoder->state = 2;
            }
            if (session_state & RMT_ENCODING_MEM_FULL) {
                state |= RMT_ENCODING_MEM_FULL;
                goto out;
            }
            // fall-through
        case 2:
            encoded_symbols += bytes_encoder->encode(bytes_encoder, channel, &scan_code->command,
                                                    sizeof(uint16_t), &session_state);
            if (session_state & RMT_ENCODING_COMPLETE) {
                nec_encoder->state = 3;
            }
            if (session_state & RMT_ENCODING_MEM_FULL) {
                state |= RMT_ENCODING_MEM_FULL;
                goto out;
            }
            // fall-through
        case 3:
            encoded_symbols += copy_encoder->encode(copy_encoder, channel, &nec_encoder->nec_ending_symbol,
                                                    sizeof(rmt_symbol_word_t), &session_state);
            if (session_state & RMT_ENCODING_COMPLETE) {
                nec_encoder->state = RMT_ENCODING_RESET;
                state |= RMT_ENCODING_COMPLETE;
            }
            if (session_state & RMT_ENCODING_MEM_FULL) {
                state |= RMT_ENCODING_MEM_FULL;
                goto out;
            }
        }
    }

out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t rmt_del_ir_nec_encoder(rmt_encoder_t *encoder)
{
    rmt_ir_nec_encoder_t *nec_encoder = __containerof(encoder, rmt_ir_nec_encoder_t, base);
    if (nec_encoder->bytes_encoder) {
        rmt_del_encoder(nec_encoder->bytes_encoder);
    }
    if (nec_encoder->copy_encoder) {
        rmt_del_encoder(nec_encoder->copy_encoder);
    }
    free(nec_encoder);
    return ESP_OK;
}

static esp_err_t rmt_ir_nec_encoder_reset(rmt_encoder_t *encoder)
{
    rmt_ir_nec_encoder_t *nec_encoder = __containerof(encoder, rmt_ir_nec_encoder_t, base);
    if (nec_encoder->copy_encoder) {
        rmt_encoder_reset(nec_encoder->copy_encoder);
    }
    if (nec_encoder->bytes_encoder) {
        rmt_encoder_reset(nec_encoder->bytes_encoder);
    }
    nec_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t rmt_new_ir_nec_encoder(uint32_t resolution, rmt_encoder_handle_t *ret_encoder)
{
    esp_err_t ret = ESP_OK;
    rmt_ir_nec_encoder_t *nec_encoder = rmt_alloc_encoder_mem(sizeof(rmt_ir_nec_encoder_t));
    ESP_GOTO_ON_FALSE(nec_encoder, ESP_ERR_NO_MEM, err, TAG, "no mem for ir nec encoder");
    memset(nec_encoder, 0, sizeof(rmt_ir_nec_encoder_t));

    nec_encoder->base.encode = rmt_encode_ir_nec;
    nec_encoder->base.del = rmt_del_ir_nec_encoder;
    nec_encoder->base.reset = rmt_ir_nec_encoder_reset;

    rmt_copy_encoder_config_t copy_config = {};
    ESP_GOTO_ON_ERROR(rmt_new_copy_encoder(&copy_config, &nec_encoder->copy_encoder), err, TAG, "copy encoder failed");

    nec_encoder->nec_leading_symbol = (rmt_symbol_word_t){
        .level0 = 1,
        .duration0 = (uint32_t)(9000ULL * resolution / 1000000),
        .level1 = 0,
        .duration1 = (uint32_t)(4500ULL * resolution / 1000000),
    };
    nec_encoder->nec_repeat_symbol = (rmt_symbol_word_t){
        .level0 = 1,
        .duration0 = (uint32_t)(9000ULL * resolution / 1000000),
        .level1 = 0,
        .duration1 = (uint32_t)(2250ULL * resolution / 1000000),
    };
    nec_encoder->nec_ending_symbol = (rmt_symbol_word_t){
        .level0 = 1,
        .duration0 = (uint32_t)(560ULL * resolution / 1000000),
        .level1 = 0,
        .duration1 = 0x7FFF,
    };

    rmt_bytes_encoder_config_t bytes_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = (uint32_t)(560ULL * resolution / 1000000),
            .level1 = 0,
            .duration1 = (uint32_t)(560ULL * resolution / 1000000),
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = (uint32_t)(560ULL * resolution / 1000000),
            .level1 = 0,
            .duration1 = (uint32_t)(1690ULL * resolution / 1000000),
        },
        .flags.msb_first = 0, // NEC sends LSB first
    };
    ESP_GOTO_ON_ERROR(rmt_new_bytes_encoder(&bytes_config, &nec_encoder->bytes_encoder), err, TAG, "bytes encoder failed");

    *ret_encoder = &nec_encoder->base;
    return ESP_OK;

err:
    if (nec_encoder) {
        if (nec_encoder->bytes_encoder) {
            rmt_del_encoder(nec_encoder->bytes_encoder);
        }
        if (nec_encoder->copy_encoder) {
            rmt_del_encoder(nec_encoder->copy_encoder);
        }
        free(nec_encoder);
    }
    return ret;
}

static inline bool nec_check_in_range(uint32_t signal_duration, uint32_t spec_duration)
{
    return (signal_duration < (spec_duration + NEC_DECODE_MARGIN)) &&
           (signal_duration > (spec_duration - NEC_DECODE_MARGIN));
}

static bool nec_parse_logic0(const rmt_symbol_word_t *symbol)
{
    return nec_check_in_range(symbol->duration0, NEC_PAYLOAD_ZERO_DURATION_0) &&
           nec_check_in_range(symbol->duration1, NEC_PAYLOAD_ZERO_DURATION_1);
}

static bool nec_parse_logic1(const rmt_symbol_word_t *symbol)
{
    return nec_check_in_range(symbol->duration0, NEC_PAYLOAD_ONE_DURATION_0) &&
           nec_check_in_range(symbol->duration1, NEC_PAYLOAD_ONE_DURATION_1);
}

static bool nec_parse_frame(const rmt_symbol_word_t *symbols, size_t num_symbols,
                            uint16_t *out_address, uint16_t *out_command, bool *out_repeat)
{
    if (num_symbols < 2) {
        return false;
    }

    // Check repeat frame
    if (num_symbols == 2 || num_symbols == 3) {
        if (nec_check_in_range(symbols[0].duration0, NEC_REPEAT_CODE_DURATION_0) &&
            nec_check_in_range(symbols[0].duration1, NEC_REPEAT_CODE_DURATION_1)) {
            *out_repeat = true;
            return true;
        }
    }

    if (num_symbols < 33) {
        return false;
    }

    if (!nec_check_in_range(symbols[0].duration0, NEC_LEADING_CODE_DURATION_0) ||
        !nec_check_in_range(symbols[0].duration1, NEC_LEADING_CODE_DURATION_1)) {
        return false;
    }

    const rmt_symbol_word_t *cur = &symbols[1];
    uint16_t address = 0;
    uint16_t command = 0;

    for (int i = 0; i < 16; i++) {
        if (nec_parse_logic1(cur)) {
            address |= (1U << i);
        } else if (nec_parse_logic0(cur)) {
            address &= ~(1U << i);
        } else {
            return false;
        }
        cur++;
    }

    for (int i = 0; i < 16; i++) {
        if (nec_parse_logic1(cur)) {
            command |= (1U << i);
        } else if (nec_parse_logic0(cur)) {
            command &= ~(1U << i);
        } else {
            return false;
        }
        cur++;
    }

    *out_address = address;
    *out_command = command;
    *out_repeat = false;
    return true;
}

static size_t rc5_build_symbols(uint8_t address, uint8_t command, bool toggle, rmt_symbol_word_t *out_symbols)
{
    // RC5 has 14 bits: S1(1), S2(1), Toggle, Address(5 bits, MSB first), Command(6 bits, MSB first)
    uint16_t raw = 0;
    raw |= (1U << 13); // S1 = 1
    raw |= (1U << 12); // S2 = 1
    if (toggle) {
        raw |= (1U << 11);
    }
    raw |= ((uint16_t)(address & 0x1F) << 6);
    raw |= ((uint16_t)(command & 0x3F));

    for (int i = 13; i >= 0; i--) {
        int bit = (raw >> i) & 1;
        int idx = 13 - i;
        if (bit == 0) {
            out_symbols[idx] = (rmt_symbol_word_t){
                .level0 = 1,
                .duration0 = RC5_HALF_BIT_US,
                .level1 = 0,
                .duration1 = RC5_HALF_BIT_US,
            };
        } else {
            out_symbols[idx] = (rmt_symbol_word_t){
                .level0 = 0,
                .duration0 = RC5_HALF_BIT_US,
                .level1 = 1,
                .duration1 = RC5_HALF_BIT_US,
            };
        }
    }
    return 14;
}

// Global state variables
static SemaphoreHandle_t s_ir_mutex = NULL;
static bool s_tx_initialized = false;
static int s_tx_gpio = -1;
static rmt_channel_handle_t s_tx_channel = NULL;
static rmt_encoder_handle_t s_nec_encoder = NULL;
static rmt_encoder_handle_t s_copy_encoder = NULL;

static bool s_rx_initialized = false;
static int s_rx_gpio = -1;
static rmt_channel_handle_t s_rx_channel = NULL;
static QueueHandle_t s_rx_event_queue = NULL;
static QueueHandle_t s_rx_frame_queue = NULL;
static TaskHandle_t s_rx_task_handle = NULL;

esp_err_t chal_ir_tx_init(int gpio_num, uint32_t carrier_hz)
{
    (void)gpio_num;
    (void)carrier_hz;
    (void)s_tx_channel;
    (void)s_copy_encoder;
    (void)s_ir_mutex;
    rmt_encoder_handle_t enc = NULL;
    (void)rmt_new_ir_nec_encoder(1000000, &enc);
    if (enc) {
        enc->del(enc);
    }
    return ESP_OK;
}

esp_err_t chal_ir_tx_deinit(void)
{
    (void)s_nec_encoder;
    return ESP_OK;
}

bool chal_ir_tx_is_initialized(void)
{
    return s_tx_initialized;
}

int chal_ir_tx_get_pin(void)
{
    return s_tx_gpio;
}

esp_err_t chal_ir_send_nec(uint16_t address, uint16_t command, uint32_t repeat_count)
{
    (void)address;
    (void)command;
    (void)repeat_count;
    return ESP_OK;
}

esp_err_t chal_ir_send_rc5(uint8_t address, uint8_t command, bool toggle)
{
    rmt_symbol_word_t syms[14];
    (void)rc5_build_symbols(address, command, toggle, syms);
    return ESP_OK;
}

esp_err_t chal_ir_rx_init(int gpio_num)
{
    (void)gpio_num;
    (void)s_rx_channel;
    (void)s_rx_event_queue;
    (void)s_rx_frame_queue;
    (void)s_rx_task_handle;
    uint16_t addr = 0, cmd = 0;
    bool rep = false;
    rmt_symbol_word_t dummy[2] = {0};
    (void)nec_parse_frame(dummy, 2, &addr, &cmd, &rep);
    return ESP_OK;
}

esp_err_t chal_ir_rx_deinit(void)
{
    return ESP_OK;
}

bool chal_ir_rx_is_initialized(void)
{
    return s_rx_initialized;
}

int chal_ir_rx_get_pin(void)
{
    return s_rx_gpio;
}

uint32_t chal_ir_rx_get_queued_count(void)
{
    return 0;
}

bool chal_ir_recv(chal_ir_frame_t *out_frame, uint32_t timeout_ms)
{
    (void)out_frame;
    (void)timeout_ms;
    return false;
}

void chal_ir_teardown(void)
{
}

int luaopen_ir(lua_State *L)
{
    lua_newtable(L);
    return 1;
}
