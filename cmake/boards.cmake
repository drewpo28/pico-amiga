# Board selection and pin maps, from pico-speccy's cmake/board.cmake and
# cmake/board-pins.cmake (same boards, same GPIO numbers).  Included BEFORE
# pico_sdk_import.cmake: it picks PICO_BOARD.  Outputs:
#   BOARD_TAG   firmware file-name prefix (pico-speccy's tags)
#   VIDEO       HDMI back-end: HSTX when the display is on GPIO 12-19
#               (video_hstx.c), PIO otherwise (video_pio.c)
#   BOARD_DEFS  compile definitions for the firmware and its drivers
#
# Boards: MURM (Murmulator 1), MURM2, PICO_PC, PICO_DV, ZERO2 (Waveshare
# RP2350-PiZero), and the carriers with a W module plugged in:
#   *_W   stock Pico 2 W / Pimoroni Pico Plus 2 W (radio on GP23/24/25/29)
#   *_WS  Waveshare RP2350B-Plus-W (radio on GPIO36-39, GP26-28 -> GPIO40-42)
# The radio is not used; a W board only moves pins.  Every board needs QSPI
# PSRAM on the XIP CS1 pin (BUTTER_PSRAM_GPIO on an RP2350A, GPIO47 on an
# RP2350B): chip RAM lives there.  MURM's PIO-SPI PSRAM (GP18-21) cannot be
# memory-mapped and is not used.
set(BOARD "MURM2" CACHE STRING
    "Target board: MURM, MURM2, PICO_PC, PICO_DV, ZERO2, or MURM/MURM2/PICO_PC/PICO_DV + _W/_WS")
set(CPU_MHZ 252 CACHE STRING "System clock in MHz (252, 378 or 504)")

string(REGEX MATCH "_(W|WS)$" W_SUFFIX "${BOARD}")
string(REGEX REPLACE "_(W|WS)$" "" CARRIER "${BOARD}")
if(NOT CARRIER MATCHES "^(MURM|MURM2|PICO_PC|PICO_DV|ZERO2)$" OR (W_SUFFIX AND CARRIER STREQUAL "ZERO2"))
    message(FATAL_ERROR "BOARD=${BOARD}: supported boards are MURM, MURM2, PICO_PC, PICO_DV, ZERO2 "
        "and MURM/MURM2/PICO_PC/PICO_DV with _W or _WS")
endif()

# ---- board header ----------------------------------------------------------------
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/../src/pico/boards)
if(CARRIER STREQUAL "ZERO2")
    set(PICO_BOARD waveshare_rp2350_pizero CACHE STRING "Board type" FORCE)
elseif(W_SUFFIX STREQUAL "_WS")
    set(PICO_BOARD pico_amiga_rp2350b_ws CACHE STRING "Board type" FORCE)
elseif(W_SUFFIX STREQUAL "_W")
    set(PICO_BOARD pico_amiga_rp2350_w CACHE STRING "Board type" FORCE)
else()
    set(PICO_BOARD pico_amiga_rp2350b CACHE STRING "Board type" FORCE)
endif()
set(PICO_PLATFORM rp2350-arm-s)

# ---- W module pin moves ----------------------------------------------------------
# W_HDR26..28: what the Pico header positions GP26/27/28 carry.
if(W_SUFFIX STREQUAL "_WS")
    set(W_HDR26 40)
    set(W_HDR27 41)
    set(W_HDR28 42)
    set(W_LED_PIN 23)               # LED2
elseif(W_SUFFIX STREQUAL "_W")
    set(W_HDR26 26)
    set(W_HDR27 27)
    set(W_HDR28 28)
    set(W_LED_PIN 255)              # the LED is on the radio (WL_GPIO0)
else()
    set(W_HDR26 26)
    set(W_HDR27 27)
    set(W_HDR28 28)
    set(W_LED_PIN 25)
endif()

# ---- tag and PSRAM CS ------------------------------------------------------------
# BUTTER_PSRAM_GPIO is the RP2350A carrier's CS1 pin; an RP2350B always uses 47.
if(CARRIER STREQUAL "MURM")
    set(BOARD_TAG "m1p2")
    set(BUTTER_PSRAM_GPIO 19)
elseif(CARRIER STREQUAL "MURM2")
    set(BOARD_TAG "m2p2")
    set(BUTTER_PSRAM_GPIO 8)
elseif(CARRIER STREQUAL "PICO_PC")
    set(BOARD_TAG "PCp2")
    set(BUTTER_PSRAM_GPIO 8)
elseif(CARRIER STREQUAL "PICO_DV")
    set(BOARD_TAG "DVp2")
    set(BUTTER_PSRAM_GPIO 47)
else()
    set(BOARD_TAG "z0p2")
    set(BUTTER_PSRAM_GPIO 47)
endif()
if(W_SUFFIX STREQUAL "_WS")
    set(BUTTER_PSRAM_GPIO 47)       # the module's PSRAM pads
    string(APPEND BOARD_TAG "ws")
elseif(W_SUFFIX STREQUAL "_W")
    string(APPEND BOARD_TAG "w")
endif()

# ---- pins ------------------------------------------------------------------------
if(CARRIER STREQUAL "MURM2")
    set(VIDEO HSTX)
    set(BOARD_DEFS
        MURM2=1
        KBD_CLOCK_PIN=2 KBD_DATA_PIN=3
        I2S_DATA_PIO=9 I2S_BCK_PIO=10 I2S_LCK_PIO=11
        PWM_PIN0=10 PWM_PIN1=11
        SDCARD_PIN_SPI0_CS=5 SDCARD_PIN_SPI0_SCK=6 SDCARD_PIN_SPI0_MOSI=7 SDCARD_PIN_SPI0_MISO=4
        USE_NESPAD NES_GPIO_CLK=20 NES_GPIO_LAT=21 NES_GPIO_DATA=${W_HDR26}   # joy2: +1
        VGA_BASE_PIN=12 HDMI_BASE_PIN=12)
elseif(CARRIER STREQUAL "PICO_PC")
    set(VIDEO HSTX)
    set(BOARD_DEFS
        PICO_PC=1
        KBD_CLOCK_PIN=0 KBD_DATA_PIN=1
        # No I2S on this board (all three pins equal = "unsupported").
        I2S_DATA_PIO=${W_HDR26} I2S_BCK_PIO=${W_HDR26} I2S_LCK_PIO=${W_HDR26}
        PWM_PIN0=${W_HDR27} PWM_PIN1=${W_HDR28}
        SDCARD_PIN_SPI0_CS=22 SDCARD_PIN_SPI0_SCK=6 SDCARD_PIN_SPI0_MOSI=7 SDCARD_PIN_SPI0_MISO=4
        USE_NESPAD NES_GPIO_CLK=5 NES_GPIO_LAT=9 NES_GPIO_DATA=20 NES_GPIO_DATA2=21
        VGA_BASE_PIN=12 HDMI_BASE_PIN=12)
elseif(CARRIER STREQUAL "PICO_DV")
    # Pimoroni Pico DV Demo Base: HDMI clock on GPIO 6/7, data 8-13 (not HSTX
    # pins); the SD card is on PIO SPI (pio1).
    set(VIDEO PIO)
    set(BOARD_DEFS
        PICO_DV=1
        KBD_CLOCK_PIN=14 KBD_DATA_PIN=15
        I2S_DATA_PIO=${W_HDR26} I2S_BCK_PIO=${W_HDR27} I2S_LCK_PIO=${W_HDR28}
        PWM_PIN0=${W_HDR26} PWM_PIN1=${W_HDR27}
        SDCARD_SPI_BUS=spi1 SDCARD_PIO=pio1 SDCARD_PIO_SM=0
        SDCARD_PIN_SPI0_CS=22 SDCARD_PIN_SPI0_SCK=5 SDCARD_PIN_SPI0_MOSI=18 SDCARD_PIN_SPI0_MISO=19
        NES_GPIO_CLK=8 NES_GPIO_LAT=9 NES_GPIO_DATA=20 NES_GPIO_DATA2=21   # no USE_NESPAD
        VGA_BASE_PIN=6 HDMI_BASE_PIN=6)
elseif(CARRIER STREQUAL "ZERO2")
    # Display on GPIO 32-39: the PIO block runs with gpio_base 16.
    set(VIDEO PIO)
    set(BOARD_DEFS
        ZERO2=1
        # PS/2 moves to the ALT pair when the PCM5122 board (I2C on 2/3) is fitted.
        KBD_CLOCK_PIN=2 KBD_DATA_PIN=3 KBD_ALT_CLOCK_PIN=14 KBD_ALT_DATA_PIN=15
        PCM5122_I2S_DATA=21 PCM5122_I2S_BCK=18 PCM5122_I2S_LCK=19
        PCM5122_I2C_SDA=2 PCM5122_I2C_SCL=3
        I2S_DATA_PIO=10 I2S_BCK_PIO=11 I2S_LCK_PIO=12
        PWM_PIN0=10 PWM_PIN1=11
        SDCARD_SPI_BUS=spi1
        SDCARD_PIN_SPI0_CS=43 SDCARD_PIN_SPI0_SCK=30 SDCARD_PIN_SPI0_MOSI=31 SDCARD_PIN_SPI0_MISO=40
        USE_NESPAD NES_GPIO_CLK=4 NES_GPIO_LAT=5 NES_GPIO_DATA=7 NES_GPIO_DATA2=8
        VGA_BASE_PIN=32 HDMI_BASE_PIN=32)
    set(W_LED_PIN 255)
else()  # MURM
    set(VIDEO PIO)
    set(BOARD_DEFS
        MURM=1
        KBD_CLOCK_PIN=0 KBD_DATA_PIN=1
        I2S_DATA_PIO=${W_HDR26} I2S_BCK_PIO=${W_HDR27} I2S_LCK_PIO=${W_HDR28}
        PWM_PIN0=${W_HDR26} PWM_PIN1=${W_HDR27}
        SDCARD_PIN_SPI0_CS=5 SDCARD_PIN_SPI0_SCK=2 SDCARD_PIN_SPI0_MOSI=3 SDCARD_PIN_SPI0_MISO=4
        USE_NESPAD NES_GPIO_CLK=14 NES_GPIO_LAT=15 NES_GPIO_DATA=16
        VGA_BASE_PIN=6 HDMI_BASE_PIN=6)
endif()
list(APPEND BOARD_DEFS
    BUTTER_PSRAM_GPIO=${BUTTER_PSRAM_GPIO}
    PICO_DEFAULT_LED_PIN=${W_LED_PIN})

message(STATUS "pico-amiga: board ${BOARD} (${BOARD_TAG}, ${PICO_BOARD}), ${VIDEO} video, ${CPU_MHZ} MHz")
