set(tool_sources "mp_tools.c" "mp_html.c" "mp_search.c" "mp_wikipedia.c" "mp_feed.c" "mp_page.c" "mp_google.c")
idf_component_register(
    SRCS ${tool_sources}
    INCLUDE_DIRS "include"
    REQUIRES micropaw_base micropaw_net
)
