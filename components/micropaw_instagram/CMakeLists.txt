idf_component_register(
    SRCS "mp_instagram.c"
    INCLUDE_DIRS "include"
    REQUIRES micropaw_base micropaw_net micropaw_agent nvs_flash heap
)
