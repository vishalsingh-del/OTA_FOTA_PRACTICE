cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.0.13")

set(EXTRA_COMPONENT_DIRS ${CMAKE_SOURCE_DIR}/src)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(OTA_FOTA_practice)