cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ArtShow)

# LVGL's PPA draw unit (lv_draw_ppa.c) sets a variable it never reads: keep
# the build free of warnings without patching the managed component
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-unused-but-set-variable)
