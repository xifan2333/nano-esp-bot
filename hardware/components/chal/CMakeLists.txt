idf_component_register(
    SRCS "src/chal_gpio.c" "src/chal_delay.c" "src/chal_wifi.c" "src/chal_nimble.c" "src/chal_ble_hid.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_system log driver freertos ledger sandbox esp_wifi esp_netif esp_event nvs_flash lwip bt esp_hid
)
