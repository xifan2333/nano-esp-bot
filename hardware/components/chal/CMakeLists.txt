idf_component_register(
    SRCS "src/chal_gpio.c" "src/chal_delay.c" "src/chal_wifi.c" "src/chal_nimble.c" "src/chal_ble_hid.c" "src/chal_ble.c" "src/chal_espnow.c" "src/chal_http.c" "src/chal_pwm.c" "src/chal_led_strip.c" "src/chal_ir.c" "src/chal_i2c.c" "src/chal_adc.c" "src/chal_button.c" "src/chal_lcd.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_system log driver freertos ledger sandbox esp_wifi esp_netif esp_event nvs_flash lwip bt esp_hid esp_http_client mbedtls storage esp_driver_ledc esp_driver_rmt esp_driver_i2c esp_adc esp_lcd
)
