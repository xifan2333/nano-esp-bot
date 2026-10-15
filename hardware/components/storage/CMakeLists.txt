idf_component_register(
    SRCS "storage.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES spiffs vfs log esp_system freertos rpc json
)
