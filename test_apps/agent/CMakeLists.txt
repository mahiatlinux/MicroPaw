cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "../../components"
    "../../components/micropaw_agent/test")
set(SDKCONFIG_DEFAULTS
    "../../sdkconfig.defaults"
    "sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(micropaw_agent_test)
