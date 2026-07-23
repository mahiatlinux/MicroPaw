idf_component_register(
    SRCS "mp_agent.c" "mp_llm.c"
    INCLUDE_DIRS "include"
    REQUIRES micropaw_base micropaw_net micropaw_tools micropaw_ota micropaw_display
    EMBED_TXTFILES "../../prompts/system.txt" "../../prompts/scheduled.txt"
)
