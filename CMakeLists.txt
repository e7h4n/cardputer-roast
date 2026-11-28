cmake_minimum_required(VERSION 3.16)

set(sdk_version_file "${CMAKE_CURRENT_SOURCE_DIR}/sdk.version")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sdk_version_file}")
file(STRINGS "${sdk_version_file}" sdk_tag LIMIT_COUNT 1)
include(FetchContent)
FetchContent_Populate(eh_agent
    GIT_REPOSITORY https://github.com/esphub-ai/esphub-sdk.git
    GIT_TAG "${sdk_tag}"
    SOURCE_DIR "${CMAKE_BINARY_DIR}/eh_agent")
set(EXTRA_COMPONENT_DIRS "${CMAKE_BINARY_DIR}/eh_agent")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# CI supplies a traceable release version; all network/OTA management belongs to ESP Hub.
if(DEFINED ENV{ROAST_VERSION} AND NOT "$ENV{ROAST_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{ROAST_VERSION}")
endif()
project(cardputer-roast)
