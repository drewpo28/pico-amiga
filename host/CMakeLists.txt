# Host build of the emulation core: the test bench (see host/main.c).
#   cmake -S host -B build-host && cmake --build build-host -j
cmake_minimum_required(VERSION 3.13)
project(pico_amiga_host C)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
include(${ROOT}/cmake/amiga_core.cmake)

add_executable(pico-amiga-host main.c)
target_link_libraries(pico-amiga-host PRIVATE amiga_core m)
target_compile_options(pico-amiga-host PRIVATE -Wall -Wextra)

# PIO TMDS word tables (src/pico/tmds_pio.h is plain C), decoded like a receiver would.
add_executable(tmds_test tests/tmds_test.c)
target_include_directories(tmds_test PRIVATE ${ROOT}/src/pico)
target_compile_options(tmds_test PRIVATE -Wall -Wextra)
