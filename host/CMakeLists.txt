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

# ---- pico-amiga: the firmware on a PC (SDL2), like the UF2 ------------------------
# src/pico/main.c and the input funnel built against host stand-ins for the Pico SDK
# (host/sdl/include) and the board drivers (host/sdl/*.cpp); see host/sdl/host_main.cpp.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 sdl2)
endif()
if(SDL2_FOUND)
    enable_language(CXX ASM)
    set(CMAKE_CXX_STANDARD 17)
    set(SDL_DIR ${CMAKE_CURRENT_LIST_DIR}/sdl)
    # HID_KEY_* usage ids from TinyUSB's hid.h, so the PC and the board share them.
    file(STRINGS ${ROOT}/external/tinyusb/src/class/hid/hid.h HID_KEY_LINES REGEX "^#define HID_KEY_")
    string(REPLACE ";" "\n" HID_KEY_TEXT "${HID_KEY_LINES}")
    file(WRITE ${CMAKE_BINARY_DIR}/gen/host_hid_keys.h "#pragma once\n${HID_KEY_TEXT}\n")

    set(FIRMWARE_SRC
        ${ROOT}/src/pico/main.c
        ${ROOT}/src/pico/input.cpp
        ${ROOT}/src/pico/machine.cpp
        ${ROOT}/src/pico/roms.c
        ${ROOT}/src/pico/drivers/input/keyboard.cpp
        ${ROOT}/src/pico/drivers/input/codepages.cpp
        ${ROOT}/src/pico/app/Config.cpp
        ${ROOT}/src/pico/ui/UiNav.cpp
        ${ROOT}/src/pico/ui/UiRender.cpp
        ${ROOT}/src/pico/ui/UiGfx.cpp
        ${ROOT}/src/pico/ui/UiFont.cpp
        ${ROOT}/src/pico/ui/UiFontCyr.cpp
        ${ROOT}/src/pico/ui/UiDialog.cpp
        ${ROOT}/src/pico/ui/UiBrowser.cpp
        ${ROOT}/src/pico/ui/UiStage.cpp
        ${ROOT}/src/pico/ui/UiTree.cpp
        ${ROOT}/src/pico/ui/UiDesc.cpp
        ${ROOT}/src/pico/ui/UiPlatform.cpp)
    set_source_files_properties(${ROOT}/src/pico/main.c PROPERTIES COMPILE_DEFINITIONS main=firmware_main)
    add_executable(pico-amiga ${FIRMWARE_SRC}
        ${SDL_DIR}/host_main.cpp
        ${SDL_DIR}/host_drivers.cpp
        ${SDL_DIR}/host_platform.cpp
        ${SDL_DIR}/host_fatfs.cpp
        ${SDL_DIR}/host_fsdir.cpp)
    include(${ROOT}/cmake/roms.cmake)
    amiga_builtin_roms(pico-amiga)
    target_include_directories(pico-amiga PRIVATE
        ${SDL_DIR}/include ${SDL_DIR} ${CMAKE_BINARY_DIR}/gen
        ${ROOT}/src/pico ${ROOT}/src/pico/ui ${ROOT}/src/pico/drivers ${ROOT}/src/pico/drivers/input
        ${ROOT}/external ${ROOT}/external/fatfs
        ${SDL2_INCLUDE_DIRS})
    # -ffunction-sections / --gc-sections as in the firmware link: pico-speccy's
    # fabgl Keyboard declares methods nothing calls and nothing defines.
    target_compile_options(pico-amiga PRIVATE
        -include ${SDL_DIR}/host_config.h -w -fno-strict-aliasing -ffunction-sections -fdata-sections)
    target_link_options(pico-amiga PRIVATE -Wl,--gc-sections)
    target_link_directories(pico-amiga PRIVATE ${SDL2_LIBRARY_DIRS})
    target_link_libraries(pico-amiga PRIVATE amiga_core ${SDL2_LIBRARIES} pthread m)
else()
    message(STATUS "host: no SDL2 (libsdl2-dev) - only the test bench is built")
endif()
