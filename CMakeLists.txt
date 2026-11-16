# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# CI sets ROAST_VERSION (v0.3.<commits on main>, or the tag); the firmware compares it with the latest release
# (main/apps/app_roast/roast_update.h). Local builds keep ESP-IDF's default (git describe).
if(DEFINED ENV{ROAST_VERSION} AND NOT "$ENV{ROAST_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{ROAST_VERSION}")
endif()
project(cardputer-roast)
