// Force-included into every firmware/driver translation unit of the SDL host build
// (host/CMakeLists.txt): what the firmware CMake passes for a board, for a board
// that has no pins.  HOST_BUILD is the one macro firmware sources may test.
#pragma once
#define HOST_BUILD 1
#define PORT_VERSION "0.1.0-host"
#define CPU_MHZ 252
#define BUTTER_PSRAM_GPIO 47
#define PICO_DEFAULT_LED_PIN 25
#define DBG_UART_TX_PIN 0          // Debug > UART log: the log is stdout (host_drivers.cpp)
