#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_pwm(lua_State *L);

esp_err_t chal_pwm_attach(int gpio_num, uint32_t freq_hz, uint8_t duty_resolution);
esp_err_t chal_pwm_write_duty(int gpio_num, uint32_t duty);
esp_err_t chal_pwm_write_ratio(int gpio_num, float ratio);
esp_err_t chal_pwm_set_servo_angle(int gpio_num, float angle_deg);
esp_err_t chal_pwm_set_servo_pulse(int gpio_num, float angle_deg, uint32_t min_us, uint32_t max_us);
esp_err_t chal_pwm_detach(int gpio_num);

bool chal_pwm_is_attached(int gpio_num);
uint32_t chal_pwm_get_duty(int gpio_num);

void chal_pwm_teardown(void);

#ifdef __cplusplus
}
#endif
