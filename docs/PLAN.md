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
      (builds on the A500+ steps below)
- [x] PIO TMDS back-end (PICO_DV); MURM1 / ZERO2 board entries still to add; VGA
- [ ] HDMI audio, I2S DAC
- [ ] Save states

## 5. A500+ (ECS)

Not "an A500 with 1 MB": Kickstart 2.04 (37.175, built in as ks204 -
kick37175.A500 is the A500+ ROM), 1 MB of chip RAM and no slow RAM, and the ECS
chipset (Agnus 8375, Denise 8373), plus a battery clock at $DC0000. A500 keeps
KS 2.04 and 3.1 in its list (real A500s took them too); A500+ lists the same two.
Machine > A500+ picks it (Config::machine, storage.nvs `machine=1`); the host
bench takes `--a500plus`; smoke.sh's test 5 (make_ecs_adf.py) covers the IDs,
the 1 MB DMA and super-hires with the free AROS ROM.

Step 1 - an A500 with 1 MB chip RAM (host):

- [x] Machine menu: an A500+ row (cmake/roms.cmake: a ROM lists its machines,
      ks204, ks205 and ks31 under A500 and A500+); Custom ROM (a Kickstart
      file from the card) on either
- [x] main.c / machine.cpp: chip_size 1 MB, no slow RAM, for the A500+ (PSRAM
      layout: chip RAM grows into the slow RAM's 512 KB); amiga_set_machine()
      switches the model from the menu (hard reset)
- [x] DMA pointers to 21 bits (`v & 0x1F` in the PTH writes): copper, blitter,
      Paula, disk, bitplanes, sprites. The A500's chip_mask still wraps them at 512 KB
- [x] VPOSR: the ECS Agnus ID ($20, PAL 8372A/8375)
- [x] Check: KS 2.04 reports 1 MB chip (Workbench: 811,424 graphics mem),
      State of the Art (needs 1 MB) runs on the A500+, the A500 screens are
      unchanged frame for frame (IK+, Cannon Fodder, State of the Art, Exolon,
      Workbench 2.04, DiagROM)

Step 2 - ECS Agnus proper (host):

- [x] BLTSIZV / BLTSIZH (blits up to 32768 x 2048 words), BLTCON0L
- [x] BEAMCON0 (stored only: PAL, its sync options are not modelled), DIWHIGH
      (H8, V8-V10; dropped by the next DIWSTRT/DIWSTOP write)
- [ ] DDF as ECS computes it (its own start/stop limits): OCS rules today
- [x] The $DC0000 clock (MSM6242B, rtc.c): the platform's time through
      cfg.rtc_time, writes to the time ignored. The host bench gives the PC's
      local time; the firmware has no time source yet (no clock fitted)

Step 3 - ECS Denise (host):

- [x] DENISEID ($FC), BPLCON3 BRDRBLNK (with BPLCON0 ECSENA)
- [x] Super-hires (1280 x 256 PAL and 1280 x 512 interlaced): a pf entry holds
      a pixel pair, its colour is the pair's two halves (COLOR[p2*4+p1]: the
      top and low bits of each gun) averaged into one output pixel - hires rows,
      no new row format. Workbench 2.04 in PAL:SuperHires and
      SuperHires-Interlaced (ScreenMode prefs) checked; pixels from DIW x 2t + 5
- [ ] Sprites over super-hires: drawn as on hires (their colours not reduced)
- [ ] BRDNTRAN, BRDSPRT, ZDBPSEL and the other BPLCON2/3 genlock bits
- [ ] Productivity (640 x 480 non-interlaced, 31 kHz on a real Amiga: a multisync
      monitor), also 4 colours: our output is 576p50, so at best it is shown
      scaled, never at its own timing
- [ ] hw: the A500+ on a board (PICO_DV: SRAM left ~5.6 KB)
