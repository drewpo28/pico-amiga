# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# pico-amiga firmware (RP2350).  Host test bench: see host/CMakeLists.txt.
#
#   cmake -B build -DPICO_SDK_PATH=/path/to/pico-sdk [-DBOARD=MURM2|PICO_DV|...] [-DCPU_MHZ=378]
#   cmake --build build -j
# Boards: cmake/boards.cmake.
cmake_minimum_required(VERSION 3.13)

include(cmake/boards.cmake)          # before the SDK: it picks PICO_BOARD
# TinyUSB 0.21 (pico-speccy's copy): its host stack and xinput_host.c are what the
# input drivers were tuned against. Must be set before pico_sdk_init().
set(PICO_TINYUSB_PATH ${CMAKE_CURRENT_LIST_DIR}/external/tinyusb)
include(pico_sdk_import.cmake)
project(pico_amiga C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

set(PORT_VERSION "0.1.0")
set(ROOT ${CMAKE_CURRENT_LIST_DIR})
include(cmake/amiga_core.cmake)
# The CPU's memory callbacks run in RAM (.time_critical.* is copied to SRAM).
target_compile_definitions(amiga_core PRIVATE
    "AMIGA_HOT=__attribute__((section(\".time_critical.amiga\")))")

include(external/fatfs/fatfs.cmake)
include(src/pico/drivers/sdcard/sdcard.cmake)

add_executable(pico-amiga
    src/pico/main.c
    src/pico/audio_pwm.c
    src/pico/clock.c
    src/pico/psram.c
    src/pico/usb_disk_stub.c
    src/pico/input.cpp
    # Input drivers from pico-speccy (PICO-AMIGA: marks local changes).
    src/pico/drivers/usbhost/hid_app.cpp
    src/pico/drivers/usbhost/joy_hid.cpp
    src/pico/drivers/usbhost/xinput_host.c
    src/pico/drivers/usbhost/hid_ri.c
    src/pico/drivers/usbhost/hid_rip.c
    src/pico/drivers/input/keyboard.cpp
    src/pico/drivers/input/codepages.cpp
    src/pico/drivers/ps2kbd/ps2kbd_mrmltr.cpp
    src/pico/drivers/nespad/nespad.cpp
)
pico_generate_pio_header(pico-amiga ${CMAKE_CURRENT_LIST_DIR}/src/pico/drivers/ps2kbd/ps2kbd_mrmltr.pio)
if(VIDEO STREQUAL "PIO")
    target_sources(pico-amiga PRIVATE src/pico/video_pio.c)
    pico_generate_pio_header(pico-amiga ${CMAKE_CURRENT_LIST_DIR}/src/pico/tmds.pio)
    target_link_libraries(pico-amiga PRIVATE hardware_pio)
else()
    target_sources(pico-amiga PRIVATE src/pico/video_hstx.c)
endif()
target_include_directories(pico-amiga PRIVATE src/pico src/pico/drivers
    src/pico/drivers/usbhost src/pico/drivers/input external)
target_compile_definitions(pico-amiga PRIVATE ${BOARD_DEFS}
    CPU_MHZ=${CPU_MHZ} PORT_VERSION="${PORT_VERSION}")
target_compile_definitions(sdcard INTERFACE ${BOARD_DEFS})
target_compile_options(pico-amiga PRIVATE -Wall -Wextra -Wno-unused-parameter)
# Third-party code (pico-speccy's drivers): keep its warnings out of ours.
set_source_files_properties(
    src/pico/drivers/usbhost/hid_app.cpp src/pico/drivers/usbhost/joy_hid.cpp
    src/pico/drivers/usbhost/xinput_host.c src/pico/drivers/usbhost/hid_ri.c
    src/pico/drivers/usbhost/hid_rip.c src/pico/drivers/input/keyboard.cpp
    src/pico/drivers/input/codepages.cpp src/pico/drivers/ps2kbd/ps2kbd_mrmltr.cpp
    src/pico/drivers/nespad/nespad.cpp
    PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(pico-amiga PRIVATE
    amiga_core
    sdcard fatfs
    pico_stdlib pico_multicore
    hardware_clocks hardware_dma hardware_pwm hardware_vreg hardware_pio
    tinyusb_host
)
set_target_properties(pico-amiga PROPERTIES OUTPUT_NAME "${BOARD_TAG}-amiga-${PORT_VERSION}")
pico_enable_stdio_uart(pico-amiga 0)
pico_enable_stdio_usb(pico-amiga 0)
pico_add_extra_outputs(pico-amiga)
