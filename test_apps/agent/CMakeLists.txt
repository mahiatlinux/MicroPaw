cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "../../components"
    "../../components/micropaw_agent/test")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(micropaw_agent_test)
