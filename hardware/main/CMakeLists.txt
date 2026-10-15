idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_hw_support esp_system esp_timer spi_flash ledger rpc)
