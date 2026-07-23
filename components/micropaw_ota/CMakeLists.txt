idf_component_register(
    SRCS "mp_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_https_ota mbedtls
)
