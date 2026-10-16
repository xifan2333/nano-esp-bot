idf_component_register(SRCS "rpc.c"
                       INCLUDE_DIRS "include"
                       REQUIRES json ledger sandbox storage
                       PRIV_REQUIRES esp_hw_support esp_driver_usb_serial_jtag driver esp_system esp_timer spi_flash)
