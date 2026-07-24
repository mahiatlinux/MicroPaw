idf_component_register(SRCS "test_llm.c" "test_policy.c"
                       PRIV_REQUIRES unity micropaw_agent
                       WHOLE_ARCHIVE)
