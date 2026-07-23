idf_component_register(
    SRCS "mp_display.c"
    INCLUDE_DIRS "include"
    REQUIRES micropaw_base esp_driver_i2c esp_driver_gpio esp_timer
)
