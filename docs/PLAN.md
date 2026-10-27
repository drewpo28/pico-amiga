# Plan

Where the skeleton stands and what comes next, roughly in order. "Host" means
verified on the host bench; "hw" means verified on a board.

## 0. Skeleton (done, host only)

- [x] Musashi 68000 with const tables (fits the RP2350), Musashi's test suite on that build
- [x] Memory map: chip/slow RAM, ROM overlay, 256/512 KB Kickstart, AROS ext ROM, CIA and custom decode
- [x] CIAs: timers (lazy E-clock sync), TOD, ICR, keyboard serial port + handshake
- [x] Copper: MOVE/WAIT/SKIP, CDANG, blitter-finished wait, PAL V8 wrap
- [x] Blitter: area (shifts, masks, minterms, inclusive/exclusive fill), line mode (octants, SING, texture)
- [x] Denise: 1-6 planes, lowres/hires, DPF, EHB, HAM6, BPLCON1 scroll, DIW, modulos, sprites (attached, priorities)
- [x] Paula: 4 DMA channels, period/volume, block interrupts; serial out
- [x] Floppy: ADF <-> MFM, disk DMA with sync, write-back, motor/step/side/CHNG/index
- [x] Host bench (PNG/WAV/serial/scripted keys) and smoke tests
- [x] Firmware: clocks, PSRAM, SD/FatFs, HSTX 720x576@50, USB HID, PWM audio (builds; not run on hw)
- [x] PIO TMDS scanout for PICO_DV (builds; word format bit-identical to pico-speccy's, host-decoded)
- [x] Native hires (720 output pixels) and interlace with both fields: per-row
      framebuffer formats, LOF toggling (reference shots: docs/screens/)

## 1. Hardware bring-up

- [ ] MURM2: picture, PSRAM detection, SD, USB keyboard/mouse, PWM audio
- [ ] Measure: frames per second at 378 and 504 MHz with DiagROM, AROS, a game
- [ ] PICO_PC: check the swapped red/green lanes and the PWM pins
- [ ] PICO_DV: PIO TMDS picture at 378 (divider 1.5) and 504; SD over PIO SPI; core 1 encode load
- [ ] UART debug console (SERDAT and firmware logs) on a free pin

## 2. Speed (expected to be needed: the host profile is ~50% bitplane fetch)

- [ ] Render lines on core 1: core 0 records per-line display state, core 1 does
      fetch + c2p + output (the pico-speccy Next split)
- [ ] c2p: byte-wise tables (8 pixels per lookup) or the SIO interpolators
- [ ] Opcode handler index (128 KB) and cycle table (64 KB) into SRAM - there is room
- [ ] m68k_execute and the hottest handlers into SRAM; profile with the SysTick sampler
- [ ] PSRAM: check XIP cache behaviour with chip RAM traffic; consider an SRAM
      copy of the most-used chip RAM page(s)

## 3. Accuracy

- [ ] DMA contention: CPU loses chip-bus cycles to bitplanes/copper/blitter
      (today the CPU runs at full speed whatever the display does)
- [ ] Copper slots taken by hires/6-plane fetches; blitter cycle timing and nasty mode
- [ ] Interlace with more than 16 colours per line (falls back to the latest field today)
- [ ] Sprite collisions (CLXDAT/CLXCON), sprite writes mid-line, sprite DMA slot limits vs DDFSTRT
- [ ] Paula: ADKCON attach modes, the LED low-pass filter, non-DMA playback
- [ ] Keyboard: power-up key stream, Ctrl-Amiga-Amiga reset, Caps Lock LED semantics
- [ ] Floppy: DF1-DF3 with drive IDs, real-speed option, DSKBYTR polling loaders
- [ ] KS 1.3 / 2.04 / 3.1 boot tests (owner's ROMs), Workbench, a set of games and demos

## 4. Features

- [ ] On-screen menu: pick ADFs from the SD card, swap disks, reset, settings
- [ ] Joystick: USB gamepads, NES/SNES pads (pico-speccy drivers)
- [ ] A600 profile: ECS Agnus/Denise, 1 MB chip RAM, KS 2.05, Gayle IDE with HDF images
- [x] PIO TMDS back-end (PICO_DV); MURM1 / ZERO2 board entries still to add; VGA
- [ ] HDMI audio, I2S DAC
- [ ] Save states
