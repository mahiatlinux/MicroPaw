idf_component_register(
    SRCS "mp_json.c" "mp_config.c" "mp_memory.c" "mp_context.c" "mp_scheduler.c" "mp_confirmation.c" "mp_metrics.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_timer heap freertos
)
