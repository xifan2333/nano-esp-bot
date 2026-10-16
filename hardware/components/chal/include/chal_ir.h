#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHAL_IR_PROTO_NEC = 0,
    CHAL_IR_PROTO_RC5,
} chal_ir_proto_t;

typedef struct {
    chal_ir_proto_t proto;
    uint16_t address;
    uint16_t command;
    bool repeat;
    bool toggle;
} chal_ir_frame_t;

/**
 * @brief Lua module registration entry point for require("ir").
 */
int luaopen_ir(lua_State *L);

/**
 * @brief Initialize IR transmitter on a GPIO with carrier frequency.
 * @param gpio_num Target GPIO number.
 * @param carrier_hz Carrier frequency in Hz (typically 38000 for NEC, 36000 for RC5; 0 defaults to 38000).
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t chal_ir_tx_init(int gpio_num, uint32_t carrier_hz);

/**
 * @brief Deinitialize IR transmitter and release resources.
 */
esp_err_t chal_ir_tx_deinit(void);

/**
 * @brief Check if IR transmitter is initialized.
 */
bool chal_ir_tx_is_initialized(void);

/**
 * @brief Get the active IR transmitter GPIO pin.
 */
int chal_ir_tx_get_pin(void);

/**
 * @brief Transmit an NEC protocol frame.
 * @param address 16-bit address (or 8-bit addr + 8-bit ~addr).
 * @param command 16-bit command (or 8-bit cmd + 8-bit ~cmd).
 * @param repeat_count Number of repeat frames to send after initial frame.
 * @return ESP_OK on success.
 */
esp_err_t chal_ir_send_nec(uint16_t address, uint16_t command, uint32_t repeat_count);

/**
 * @brief Transmit an RC5 protocol frame.
 * @param address 5-bit address (0~31).
 * @param command 6-bit command (0~63).
 * @param toggle Toggle bit.
 * @return ESP_OK on success.
 */
esp_err_t chal_ir_send_rc5(uint8_t address, uint8_t command, bool toggle);

/**
 * @brief Initialize IR receiver on a GPIO.
 * @param gpio_num Target GPIO connected to demodulated IR receiver (e.g. VS1838B).
 * @return ESP_OK on success.
 */
esp_err_t chal_ir_rx_init(int gpio_num);

/**
 * @brief Deinitialize IR receiver and release resources.
 */
esp_err_t chal_ir_rx_deinit(void);

/**
 * @brief Check if IR receiver is initialized.
 */
bool chal_ir_rx_is_initialized(void);

/**
 * @brief Get active IR receiver GPIO pin.
 */
int chal_ir_rx_get_pin(void);

/**
 * @brief Get number of frames currently pending in RX queue.
 */
uint32_t chal_ir_rx_get_queued_count(void);

/**
 * @brief Receive an IR frame from the RX queue.
 * @param out_frame Pointer to destination frame struct.
 * @param timeout_ms Timeout in milliseconds (0 for non-blocking).
 * @return true if a frame was retrieved, false on timeout or empty.
 */
bool chal_ir_recv(chal_ir_frame_t *out_frame, uint32_t timeout_ms);

/**
 * @brief Emergency teardown hook for supervisor break / abort.
 */
void chal_ir_teardown(void);

#ifdef __cplusplus
}
#endif
