cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
# Memory counters in the component, reported by /stats
idf_build_set_property(COMPILE_DEFINITIONS "CFT_DEBUG_STATS" APPEND)
project(cft_device_test)
