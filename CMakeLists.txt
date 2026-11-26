# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/.deps/esphub-firmware/firmware/components/eh_agent")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# CI supplies a traceable release version; all network/OTA management belongs to ESP Hub.
if(DEFINED ENV{ROAST_VERSION} AND NOT "$ENV{ROAST_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{ROAST_VERSION}")
endif()
project(cardputer-roast)
