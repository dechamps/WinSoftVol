cmake_minimum_required(VERSION 3.20)
project(WinSoftVol)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/FindWDK/cmake")

find_package(WDK REQUIRED)

wdk_add_driver(WinSoftVol KMDF 1.11 driver.c)
target_compile_options(WinSoftVol PRIVATE /external:anglebrackets /WX /W4 /external:W0 /permissive- /analyze /analyze:external-)
