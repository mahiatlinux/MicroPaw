set(tool_sources "mp_tools.c" "mp_html.c" "mp_search.c" "mp_wikipedia.c" "mp_feed.c" "mp_page.c")
if(CONFIG_MICROPAW_GMAIL OR CONFIG_MICROPAW_CALENDAR)
    list(APPEND tool_sources "mp_google.c")
endif()
if(CONFIG_MICROPAW_GMAIL)
    list(APPEND tool_sources "mp_gmail.c")
endif()
if(CONFIG_MICROPAW_CALENDAR)
    list(APPEND tool_sources "mp_calendar.c")
endif()
idf_component_register(
    SRCS ${tool_sources}
    INCLUDE_DIRS "include"
    REQUIRES micropaw_base micropaw_net
)
