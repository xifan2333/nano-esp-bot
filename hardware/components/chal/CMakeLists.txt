idf_component_register(
    SRCS "src/chal_gpio.c" "src/chal_delay.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_system log driver freertos ledger sandbox
)
