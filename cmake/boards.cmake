# Board pin maps, taken from pico-speccy's cmake/board-pins.cmake for the
# boards whose HDMI sits on GPIO 12-19, i.e. the ones the HSTX scanout can
# drive.  Everything else needs the PIO TMDS back-end (not ported yet).
set(BOARD "MURM2" CACHE STRING "Target board: MURM2 or PICO_PC")
set(CPU_MHZ 378 CACHE STRING "System clock in MHz (252, 378 or 504)")

if(BOARD STREQUAL "MURM2")
    set(BOARD_TAG "m2p2")
    set(BOARD_DEFS
        MURM2=1
        HDMI_BASE_PIN=12
        SDCARD_PIN_SPI0_CS=5 SDCARD_PIN_SPI0_SCK=6 SDCARD_PIN_SPI0_MOSI=7 SDCARD_PIN_SPI0_MISO=4
        PWM_PIN0=10 PWM_PIN1=11
        BUTTER_PSRAM_GPIO=8)
elseif(BOARD STREQUAL "PICO_PC")
    set(BOARD_TAG "PCp2")
    set(BOARD_DEFS
        PICO_PC=1
        HDMI_BASE_PIN=12
        SDCARD_PIN_SPI0_CS=22 SDCARD_PIN_SPI0_SCK=6 SDCARD_PIN_SPI0_MOSI=7 SDCARD_PIN_SPI0_MISO=4
        PWM_PIN0=27 PWM_PIN1=28
        BUTTER_PSRAM_GPIO=8)
else()
    message(FATAL_ERROR "BOARD=${BOARD}: only MURM2 and PICO_PC (HDMI on GPIO 12-19, HSTX) "
        "are supported so far. PICO_DV / MURM1 / ZERO2 need the PIO TMDS back-end.")
endif()
message(STATUS "pico-amiga: board ${BOARD} (${BOARD_TAG}), ${CPU_MHZ} MHz")
