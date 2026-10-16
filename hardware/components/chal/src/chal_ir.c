#include "chal_ir.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
    (void)data_size;
    rmt_ir_nec_encoder_t *nec_encoder = __containerof(encoder, rmt_ir_nec_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;
    const ir_nec_scan_code_t *scan_code = (const ir_nec_scan_code_t *)primary_data;
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

static bool rc5_parse_frame(const rmt_symbol_word_t *symbols, size_t num_symbols,
                            uint8_t *out_address, uint8_t *out_command, bool *out_toggle)
{
    if (num_symbols < 7 || num_symbols > 30) {
        return false;
    }

    // Demodulated RC5 is active low.
    // Convert duration stream into half-bit levels (0=active carrier, 1=idle space)
    uint8_t half_levels[32];
    size_t half_idx = 0;

    for (size_t s = 0; s < num_symbols && half_idx < 32; s++) {
        uint32_t d0 = symbols[s].duration0;
        uint32_t d1 = symbols[s].duration1;

        int ticks0 = (int)((d0 + (RC5_HALF_BIT_US / 2)) / RC5_HALF_BIT_US);
        if (ticks0 < 1 || ticks0 > 2) {
            return false;
        }
        for (int k = 0; k < ticks0 && half_idx < 32; k++) {
            half_levels[half_idx++] = symbols[s].level0;
        }

        if (d1 > 0 && d1 < 5000) {
            int ticks1 = (int)((d1 + (RC5_HALF_BIT_US / 2)) / RC5_HALF_BIT_US);
            if (ticks1 < 1 || ticks1 > 2) {
                return false;
            }
            for (int k = 0; k < ticks1 && half_idx < 32; k++) {
                half_levels[half_idx++] = symbols[s].level1;
            }
        }
    }

    if (half_idx < 26) {
        return false;
    }

    uint16_t decoded_bits = 0;
    for (size_t b = 0; b < 14; b++) {
        if (2 * b + 1 >= half_idx) {
            // Trailing half-bit may not be recorded if receiver goes idle
            break;
        }
        uint8_t h0 = half_levels[2 * b];
        uint8_t h1 = half_levels[2 * b + 1];
        if (h0 == 0 && h1 == 1) {
            // Logic 0
            decoded_bits = (decoded_bits << 1);
        } else if (h0 == 1 && h1 == 0) {
            // Logic 1
            decoded_bits = (decoded_bits << 1) | 1;
        } else {
            return false;
        }
    }

    // Verify S1 = 1
    if (!((decoded_bits >> 13) & 1)) {
        return false;
    }

    *out_toggle = ((decoded_bits >> 11) & 1) ? true : false;
    *out_address = (uint8_t)((decoded_bits >> 6) & 0x1F);
    *out_command = (uint8_t)(decoded_bits & 0x3F);
    return true;
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
static rmt_symbol_word_t s_rx_raw_symbols[IR_RX_MEM_BLOCK_SYMBOLS];

static const rmt_receive_config_t s_rx_receive_cfg = {
    .signal_range_min_ns = 1250,     // 1.25us min pulse (avoids HF glitches)
    .signal_range_max_ns = 12000000, // 12ms max duration (accommodates 9ms lead pulse + tolerance)
};

static bool ir_rmt_rx_done_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_data)
{
    (void)channel;
    BaseType_t high_task_wakeup = pdFALSE;
    QueueHandle_t queue = (QueueHandle_t)user_data;
    if (queue) {
        xQueueSendFromISR(queue, edata, &high_task_wakeup);
    }
    return high_task_wakeup == pdTRUE;
}

static void ir_rx_parser_task(void *pvParameters)
{
    (void)pvParameters;
    rmt_rx_done_event_data_t rx_data;

    while (s_rx_initialized) {
        if (xQueueReceive(s_rx_event_queue, &rx_data, pdMS_TO_TICKS(50)) == pdPASS) {
            if (rx_data.num_symbols > 0 && rx_data.received_symbols != NULL) {
                chal_ir_frame_t frame;
                memset(&frame, 0, sizeof(frame));
                uint16_t nec_addr = 0, nec_cmd = 0;
                bool nec_rep = false;
                uint8_t rc5_addr = 0, rc5_cmd = 0;
                bool rc5_tog = false;

                if (nec_parse_frame(rx_data.received_symbols, rx_data.num_symbols,
                                    &nec_addr, &nec_cmd, &nec_rep)) {
                    frame.proto = CHAL_IR_PROTO_NEC;
                    frame.address = nec_addr;
                    frame.command = nec_cmd;
                    frame.repeat = nec_rep;
                    if (s_rx_frame_queue) {
                        if (uxQueueSpacesAvailable(s_rx_frame_queue) == 0) {
                            chal_ir_frame_t drop;
                            xQueueReceive(s_rx_frame_queue, &drop, 0);
                        }
                        xQueueSend(s_rx_frame_queue, &frame, 0);
                    }
                } else if (rc5_parse_frame(rx_data.received_symbols, rx_data.num_symbols,
                                           &rc5_addr, &rc5_cmd, &rc5_tog)) {
                    frame.proto = CHAL_IR_PROTO_RC5;
                    frame.address = rc5_addr;
                    frame.command = rc5_cmd;
                    frame.toggle = rc5_tog;
                    if (s_rx_frame_queue) {
                        if (uxQueueSpacesAvailable(s_rx_frame_queue) == 0) {
                            chal_ir_frame_t drop;
                            xQueueReceive(s_rx_frame_queue, &drop, 0);
                        }
                        xQueueSend(s_rx_frame_queue, &frame, 0);
                    }
                }
            }
            if (s_rx_initialized && s_rx_channel) {
                rmt_receive(s_rx_channel, s_rx_raw_symbols, sizeof(s_rx_raw_symbols), &s_rx_receive_cfg);
            }
        }
    }
    vTaskDelete(NULL);
}

esp_err_t chal_ir_tx_init(int gpio_num, uint32_t carrier_hz)
{
    if (!ledger_is_pin_safe(gpio_num)) {
        ESP_LOGE(TAG, "GPIO %d is not safe for IR TX", gpio_num);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_ir_mutex) {
        s_ir_mutex = xSemaphoreCreateMutex();
        if (!s_ir_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_ir_mutex, portMAX_DELAY);
    if (s_tx_initialized) {
        xSemaphoreGive(s_ir_mutex);
        ESP_LOGW(TAG, "IR TX already initialized on GPIO %d", s_tx_gpio);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ledger_claim_pin(gpio_num, LEDGER_OWNER_SCRIPT, LEDGER_PIN_MODE_RMT, "ir_tx");
    if (err != ESP_OK) {
        xSemaphoreGive(s_ir_mutex);
        ESP_LOGE(TAG, "Failed to claim GPIO %d for IR TX: %s", gpio_num, esp_err_to_name(err));
        return err;
    }

    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
        .gpio_num = gpio_num,
    };
    err = rmt_new_tx_channel(&tx_cfg, &s_tx_channel);
    if (err != ESP_OK) {
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_ir_mutex);
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    rmt_carrier_config_t carrier_cfg = {
        .duty_cycle = 0.33f,
        .frequency_hz = carrier_hz ? carrier_hz : IR_NEC_CARRIER_HZ,
    };
    err = rmt_apply_carrier(s_tx_channel, &carrier_cfg);
    if (err != ESP_OK) {
        rmt_del_channel(s_tx_channel);
        s_tx_channel = NULL;
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_ir_mutex);
        ESP_LOGE(TAG, "rmt_apply_carrier failed: %s", esp_err_to_name(err));
        return err;
    }

    err = rmt_new_ir_nec_encoder(IR_RESOLUTION_HZ, &s_nec_encoder);
    if (err != ESP_OK) {
        rmt_del_channel(s_tx_channel);
        s_tx_channel = NULL;
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_ir_mutex);
        ESP_LOGE(TAG, "rmt_new_ir_nec_encoder failed: %s", esp_err_to_name(err));
        return err;
    }

    rmt_copy_encoder_config_t copy_cfg = {};
    err = rmt_new_copy_encoder(&copy_cfg, &s_copy_encoder);
    if (err != ESP_OK) {
        s_nec_encoder->del(s_nec_encoder);
        s_nec_encoder = NULL;
        rmt_del_channel(s_tx_channel);
        s_tx_channel = NULL;
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_ir_mutex);
        ESP_LOGE(TAG, "rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
        return err;
    }

    err = rmt_enable(s_tx_channel);
    if (err != ESP_OK) {
        s_copy_encoder->del(s_copy_encoder);
        s_copy_encoder = NULL;
        s_nec_encoder->del(s_nec_encoder);
        s_nec_encoder = NULL;
        rmt_del_channel(s_tx_channel);
        s_tx_channel = NULL;
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_ir_mutex);
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    s_tx_initialized = true;
    s_tx_gpio = gpio_num;
    xSemaphoreGive(s_ir_mutex);
    ESP_LOGI(TAG, "IR TX initialized on GPIO %d (carrier %lu Hz)", gpio_num, (unsigned long)carrier_cfg.frequency_hz);
    return ESP_OK;
}

esp_err_t chal_ir_tx_deinit(void)
{
    if (!s_ir_mutex) {
        return ESP_OK;
    }

    xSemaphoreTake(s_ir_mutex, portMAX_DELAY);
    if (!s_tx_initialized) {
        xSemaphoreGive(s_ir_mutex);
        return ESP_OK;
    }

    if (s_tx_channel) {
        rmt_disable(s_tx_channel);
        rmt_del_channel(s_tx_channel);
        s_tx_channel = NULL;
    }
    if (s_nec_encoder) {
        s_nec_encoder->del(s_nec_encoder);
        s_nec_encoder = NULL;
    }
    if (s_copy_encoder) {
        s_copy_encoder->del(s_copy_encoder);
        s_copy_encoder = NULL;
    }
    if (s_tx_gpio >= 0) {
        ledger_release_pin(s_tx_gpio, LEDGER_OWNER_SCRIPT);
        s_tx_gpio = -1;
    }
    s_tx_initialized = false;
    xSemaphoreGive(s_ir_mutex);
    ESP_LOGI(TAG, "IR TX deinitialized");
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
    if (!s_tx_initialized || !s_tx_channel || !s_nec_encoder) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_ir_mutex, portMAX_DELAY);
    rmt_ir_nec_encoder_t *enc = __containerof(s_nec_encoder, rmt_ir_nec_encoder_t, base);
    enc->is_repeat = false;
    s_nec_encoder->reset(s_nec_encoder);

    ir_nec_scan_code_t scan_code = {
        .address = address,
        .command = command,
    };
    rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
    };

    esp_err_t err = rmt_transmit(s_tx_channel, s_nec_encoder, &scan_code, sizeof(scan_code), &transmit_config);
    if (err != ESP_OK) {
        xSemaphoreGive(s_ir_mutex);
        ESP_LOGE(TAG, "rmt_transmit NEC failed: %s", esp_err_to_name(err));
        return err;
    }

    rmt_tx_wait_all_done(s_tx_channel, 200);

    for (uint32_t i = 0; i < repeat_count; i++) {
        vTaskDelay(pdMS_TO_TICKS(40));
        enc->is_repeat = true;
        s_nec_encoder->reset(s_nec_encoder);
        err = rmt_transmit(s_tx_channel, s_nec_encoder, &scan_code, sizeof(scan_code), &transmit_config);
        if (err != ESP_OK) {
            break;
        }
        rmt_tx_wait_all_done(s_tx_channel, 200);
    }

    xSemaphoreGive(s_ir_mutex);
    return err;
}

esp_err_t chal_ir_send_rc5(uint8_t address, uint8_t command, bool toggle)
{
    if (!s_tx_initialized || !s_tx_channel || !s_copy_encoder) {
        return ESP_ERR_INVALID_STATE;
    }

    rmt_symbol_word_t syms[14];
    rc5_build_symbols(address, command, toggle, syms);

    xSemaphoreTake(s_ir_mutex, portMAX_DELAY);
    s_copy_encoder->reset(s_copy_encoder);
    rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
    };
    esp_err_t err = rmt_transmit(s_tx_channel, s_copy_encoder, syms, sizeof(syms), &transmit_config);
    if (err != ESP_OK) {
        xSemaphoreGive(s_ir_mutex);
        ESP_LOGE(TAG, "rmt_transmit RC5 failed: %s", esp_err_to_name(err));
        return err;
    }
    rmt_tx_wait_all_done(s_tx_channel, 200);
    xSemaphoreGive(s_ir_mutex);
    return ESP_OK;
}

esp_err_t chal_ir_rx_init(int gpio_num)
{
    if (!ledger_is_pin_safe(gpio_num)) {
        ESP_LOGE(TAG, "GPIO %d is not safe for IR RX", gpio_num);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_ir_mutex) {
        s_ir_mutex = xSemaphoreCreateMutex();
        if (!s_ir_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_ir_mutex, portMAX_DELAY);
    if (s_rx_initialized) {
        xSemaphoreGive(s_ir_mutex);
        ESP_LOGW(TAG, "IR RX already initialized on GPIO %d", s_rx_gpio);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ledger_claim_pin(gpio_num, LEDGER_OWNER_SCRIPT, LEDGER_PIN_MODE_RMT, "ir_rx");
    if (err != ESP_OK) {
        xSemaphoreGive(s_ir_mutex);
        ESP_LOGE(TAG, "Failed to claim GPIO %d for IR RX: %s", gpio_num, esp_err_to_name(err));
        return err;
    }

    s_rx_event_queue = xQueueCreate(4, sizeof(rmt_rx_done_event_data_t));
    s_rx_frame_queue = xQueueCreate(IR_RX_QUEUE_DEPTH, sizeof(chal_ir_frame_t));
    if (!s_rx_event_queue || !s_rx_frame_queue) {
        if (s_rx_event_queue) {
            vQueueDelete(s_rx_event_queue);
            s_rx_event_queue = NULL;
        }
        if (s_rx_frame_queue) {
            vQueueDelete(s_rx_frame_queue);
            s_rx_frame_queue = NULL;
        }
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_ir_mutex);
        return ESP_ERR_NO_MEM;
    }

    rmt_rx_channel_config_t rx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = IR_RX_MEM_BLOCK_SYMBOLS,
        .gpio_num = gpio_num,
    };
    err = rmt_new_rx_channel(&rx_cfg, &s_rx_channel);
    if (err != ESP_OK) {
        vQueueDelete(s_rx_event_queue);
        s_rx_event_queue = NULL;
        vQueueDelete(s_rx_frame_queue);
        s_rx_frame_queue = NULL;
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_ir_mutex);
        ESP_LOGE(TAG, "rmt_new_rx_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = ir_rmt_rx_done_callback,
    };
    err = rmt_rx_register_event_callbacks(s_rx_channel, &cbs, s_rx_event_queue);
    if (err != ESP_OK) {
        rmt_del_channel(s_rx_channel);
        s_rx_channel = NULL;
        vQueueDelete(s_rx_event_queue);
        s_rx_event_queue = NULL;
        vQueueDelete(s_rx_frame_queue);
        s_rx_frame_queue = NULL;
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_ir_mutex);
        return err;
    }

    err = rmt_enable(s_rx_channel);
    if (err != ESP_OK) {
        rmt_del_channel(s_rx_channel);
        s_rx_channel = NULL;
        vQueueDelete(s_rx_event_queue);
        s_rx_event_queue = NULL;
        vQueueDelete(s_rx_frame_queue);
        s_rx_frame_queue = NULL;
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_ir_mutex);
        return err;
    }

    s_rx_initialized = true;
    s_rx_gpio = gpio_num;

    // Start background receiver task
    BaseType_t task_ret = xTaskCreatePinnedToCore(ir_rx_parser_task, "ir_rx_task", 4096, NULL, 10, &s_rx_task_handle, 1);
    if (task_ret != pdPASS) {
        s_rx_initialized = false;
        rmt_disable(s_rx_channel);
        rmt_del_channel(s_rx_channel);
        s_rx_channel = NULL;
        vQueueDelete(s_rx_event_queue);
        s_rx_event_queue = NULL;
        vQueueDelete(s_rx_frame_queue);
        s_rx_frame_queue = NULL;
        ledger_release_pin(gpio_num, LEDGER_OWNER_SCRIPT);
        xSemaphoreGive(s_ir_mutex);
        return ESP_ERR_NO_MEM;
    }

    // Arm the first reception
    rmt_receive(s_rx_channel, s_rx_raw_symbols, sizeof(s_rx_raw_symbols), &s_rx_receive_cfg);

    xSemaphoreGive(s_ir_mutex);
    ESP_LOGI(TAG, "IR RX initialized on GPIO %d", gpio_num);
    return ESP_OK;
}

esp_err_t chal_ir_rx_deinit(void)
{
    if (!s_ir_mutex) {
        return ESP_OK;
    }

    xSemaphoreTake(s_ir_mutex, portMAX_DELAY);
    if (!s_rx_initialized) {
        xSemaphoreGive(s_ir_mutex);
        return ESP_OK;
    }

    s_rx_initialized = false;

    if (s_rx_task_handle) {
        vTaskDelete(s_rx_task_handle);
        s_rx_task_handle = NULL;
    }

    if (s_rx_channel) {
        rmt_disable(s_rx_channel);
        rmt_del_channel(s_rx_channel);
        s_rx_channel = NULL;
    }

    if (s_rx_event_queue) {
        vQueueDelete(s_rx_event_queue);
        s_rx_event_queue = NULL;
    }

    if (s_rx_frame_queue) {
        vQueueDelete(s_rx_frame_queue);
        s_rx_frame_queue = NULL;
    }

    if (s_rx_gpio >= 0) {
        ledger_release_pin(s_rx_gpio, LEDGER_OWNER_SCRIPT);
        s_rx_gpio = -1;
    }

    xSemaphoreGive(s_ir_mutex);
    ESP_LOGI(TAG, "IR RX deinitialized");
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
    if (!s_rx_initialized || !s_rx_frame_queue) {
        return 0;
    }
    return (uint32_t)uxQueueMessagesWaiting(s_rx_frame_queue);
}

bool chal_ir_recv(chal_ir_frame_t *out_frame, uint32_t timeout_ms)
{
    if (!s_rx_initialized || !s_rx_frame_queue || !out_frame) {
        return false;
    }

    TickType_t wait_ticks = timeout_ms ? pdMS_TO_TICKS(timeout_ms) : 0;
    return xQueueReceive(s_rx_frame_queue, out_frame, wait_ticks) == pdPASS;
}

void chal_ir_teardown(void)
{
    chal_ir_tx_deinit();
    chal_ir_rx_deinit();
}

int luaopen_ir(lua_State *L)
{
    lua_newtable(L);
    return 1;
}
