# Board pin maps, taken from pico-speccy's cmake/board-pins.cmake.
# VIDEO picks the HDMI back-end: HSTX when the display is on GPIO 12-19
# (video_hstx.c), PIO otherwise (video_pio.c).
set(BOARD "MURM2" CACHE STRING "Target board: MURM2, PICO_PC or PICO_DV")
set(CPU_MHZ 378 CACHE STRING "System clock in MHz (252, 378 or 504)")

if(BOARD STREQUAL "MURM2")
    set(BOARD_TAG "m2p2")
    set(VIDEO HSTX)
    set(BOARD_DEFS
        MURM2=1
        HDMI_BASE_PIN=12
        SDCARD_PIN_SPI0_CS=5 SDCARD_PIN_SPI0_SCK=6 SDCARD_PIN_SPI0_MOSI=7 SDCARD_PIN_SPI0_MISO=4
        PWM_PIN0=10 PWM_PIN1=11
        BUTTER_PSRAM_GPIO=8)
elseif(BOARD STREQUAL "PICO_PC")
    set(BOARD_TAG "PCp2")
    set(VIDEO HSTX)
    set(BOARD_DEFS
        PICO_PC=1
        HDMI_BASE_PIN=12
        SDCARD_PIN_SPI0_CS=22 SDCARD_PIN_SPI0_SCK=6 SDCARD_PIN_SPI0_MOSI=7 SDCARD_PIN_SPI0_MISO=4
        PWM_PIN0=27 PWM_PIN1=28
        BUTTER_PSRAM_GPIO=8)
elseif(BOARD STREQUAL "PICO_DV")
    # Pimoroni Pico DV Demo Base + Pico Plus 2 (PSRAM on GPIO 47). HDMI clock on
    # GPIO 6/7, data 8-13: not HSTX pins. The SD card is on PIO SPI (pio1).
    set(BOARD_TAG "DVp2")
    set(VIDEO PIO)
    set(BOARD_DEFS
        PICO_DV=1
        HDMI_BASE_PIN=6
        SDCARD_SPI_BUS=spi1 SDCARD_PIO=pio1 SDCARD_PIO_SM=0
        SDCARD_PIN_SPI0_CS=22 SDCARD_PIN_SPI0_SCK=5 SDCARD_PIN_SPI0_MOSI=18 SDCARD_PIN_SPI0_MISO=19
        PWM_PIN0=26 PWM_PIN1=27
        BUTTER_PSRAM_GPIO=47)
else()
    message(FATAL_ERROR "BOARD=${BOARD}: supported boards are MURM2, PICO_PC and PICO_DV")
endif()
message(STATUS "pico-amiga: board ${BOARD} (${BOARD_TAG}), ${VIDEO} video, ${CPU_MHZ} MHz")
