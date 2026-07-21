idf_component_register(
    SRCS "mp_net.c" "mp_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client mbedtls esp_wifi esp_netif esp_event lwip micropaw_base
)
