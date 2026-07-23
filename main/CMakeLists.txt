idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES micropaw_base micropaw_net micropaw_tools micropaw_agent micropaw_telegram micropaw_ota
             esp_psram esp_driver_usb_serial_jtag
)
