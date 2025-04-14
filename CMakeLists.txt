# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# First include the ESP-IDF project.cmake and define your project
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(BLE_WIFI)

# After project() is defined, you can set global compile options
add_compile_options(-Os -fno-jump-tables -fno-tree-switch-conversion -ffunction-sections -fdata-sections -mno-text-section-literals)

# Add global linker options
add_link_options(-Wl,--gc-sections)