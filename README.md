# pico-amiga

An Amiga 500 (OCS, PAL) emulator for RP2350 boards with QSPI PSRAM, in the
family of [pico-speccy](https://github.com/drewpo28/pico-speccy).

> **Status: skeleton.** The emulation core boots DiagROM and AROS and loads a
> bootblock from an ADF on the host test bench. The firmware builds for MURM2,
> Olimex PICO-PC and Pimoroni Pico DV, but **nobody has run it on hardware yet**,
> and speed on the RP2350 has not been measured.

Verified on the host (`host/tests/smoke.sh`):

| Test | What it exercises |
|---|---|
| DiagROM 1.3 reaches its main menu | 68000, chip RAM detection, CIAs, serial, copper, hires bitplanes |
| DiagROM keyboard + "play test-module" | keyboard handshake via CIA-A SP, Paula DMA audio, CIA timer interrupts |
| AROS shows its boot screen | blitter (area + line), sprites, 640-px interlaced screen |
| AROS boots a test ADF's bootblock | CIA-B drive control, MFM track encoder, disk DMA with sync, trackdisk |

## Hardware

| | |
|---|---|
| MCU | RP2350 (A or B), clocked at 378 MHz (252/504 also selectable) |
| PSRAM | QSPI "butter" PSRAM on XIP CS1, **at least 2 MB**: GPIO 8 on RP2350A carriers, GPIO 47 on RP2350B modules (Pimoroni Pico Plus 2) |
| Video | HDMI/DVI, 720x576 @ 50 Hz, the 360x288 Amiga image pixel-doubled: through HSTX when the display is on GPIO 12-19, through a PIO TMDS serializer otherwise |
| Storage | SD card over SPI (FatFs; PIO SPI on PICO_DV) |
| Input | USB keyboard and mouse (TinyUSB host, boot protocol) |
| Audio | PWM on two pins (RC filter expected) |

| Board | `-DBOARD=` | Video | Notes |
|---|---|---|---|
| Murmulator 2.0 | `MURM2` (default) | HSTX, GPIO 12-19 | |
| Olimex RP2040-PICO-PC + Pico 2 | `PICO_PC` | HSTX, GPIO 12-19 | red/green lanes swapped |
| Pimoroni Pico DV Demo Base + Pico Plus 2 | `PICO_DV` | PIO, GPIO 6-13 | SD on PIO SPI; needs the Plus 2's PSRAM |

Pin maps are pico-speccy's. MURM1 and ZERO2 would be a board entry each in
`cmake/boards.cmake` (MURM1 uses the same PIO back-end).

## SD card

```
/amiga/kick.rom   Kickstart, 256 or 512 KB (required). AROS works.
/amiga/ext.rom    512 KB extended ROM at $E00000 (optional; AROS needs it)
/amiga/df0.adf    disk in DF0 (optional; streamed from the card, written back)
```

The machine is an A500 with 512 KB chip RAM and 512 KB slow RAM at $C00000.
F12 resets it. If the board cannot start, the screen turns one colour and the LED
blinks: red = no PSRAM (or less than 2 MB), magenta = no SD card, blue = no
`kick.rom`.

## Building

Firmware (needs the Pico SDK 2.x and an Arm toolchain):

```sh
cmake -B build -DPICO_SDK_PATH=/path/to/pico-sdk -DBOARD=MURM2   # or PICO_PC, PICO_DV; -DCPU_MHZ=504
cmake --build build -j
# -> build/m2p2-amiga-0.1.0.uf2
```

Host test bench (plain C compiler, no SDK):

```sh
cmake -S host -B build-host && cmake --build build-host -j
host/tests/fetch_test_roms.sh roms      # AROS + DiagROM, from the vAmiga repository
host/tests/smoke.sh roms                # DiagROM, keyboard, audio, AROS floppy boot
build-host/pico-amiga-host --rom roms/diagrom-13.bin --frames 1500 --shot 1500:diag.png
```

`pico-amiga-host` writes framebuffer snapshots (`--shot N:file.png`), Paula
output (`--wav`), prints SERDAT to stdout and can type keys (`--key frame:code:down`).

## How it is built

```
src/amiga/     the emulator: platform-independent C, shared by firmware and host
  amiga.c        reset, line/frame scheduler, 68000 glue, keyboard controller
  mem.c          24-bit map in 64 KB banks, Musashi memory callbacks
  custom.c       $DFF000 register file: DMACON, INTENA/INTREQ, beam, ports
  copper.c       MOVE/WAIT/SKIP against the beam
  blitter.c      area mode (shift, masks, minterms, fill) and line mode
  denise.c       bitplane + sprite DMA and the pixel pipeline
  paula.c        4-channel DMA audio
  cia.c          two 8520s (timers, TOD, ICR, keyboard serial port)
  floppy.c       drives, ADF <-> MFM track codec, disk DMA
src/pico/      RP2350 firmware: main, video (HSTX / PIO TMDS), PSRAM, clocks, USB input, PWM audio
host/          test bench and smoke tests
external/      Musashi 68000 core (+ generated const opcode tables), FatFs
tools/         gen_musashi.py (regenerates external/musashi/gen)
docs/PLAN.md   what is missing and in which order
```

The main ideas are in the file headers; in short:

* **CPU**: Musashi 4.x, configured as a plain 68000. Its 256 KB jump table and
  320 KB cycle tables would not fit the RP2350's SRAM, so `tools/gen_musashi.py`
  turns them into const arrays (flash). Musashi's own 68000 test suite passes on
  that build (59 of 60; `move.bin` fails identically on stock Musashi in 68000 mode).
* **Time**: a line is 227 colour clocks; the copper and the CPU take turns in
  16-cc slices. Every display-register write carries its own beam position.
* **Display**: Denise renders the current line incrementally: a register write
  first draws everything the beam has passed with the old value. Copper bars,
  mid-line BPLCON changes and splits come out pixel-exact without an event queue.
* **Memory**: chip RAM, slow RAM and the ROMs live in PSRAM, memory-mapped
  through the XIP cache; the framebuffer (360x288 x 16 bit) is in SRAM.

## Credits and licences

GPL-3.0 (see LICENSE), like pico-speccy, whose PSRAM, clock and board code this
reuses (the PIO TMDS pin format and balanced character pairs, `tmds_pair.h`,
are pico-speccy's too). Third-party parts keep their licences: Musashi (MIT, Karl Stenerud),
FatFs (ChaN), the SD card driver (BSD-2, Elehobica, via pico-speccy), the HSTX
scanout structure (BSD-3, Raspberry Pi pico-examples). Hardware behaviour was
checked against the Amiga Hardware Reference Manual and, where it is vague,
against how vAmiga models it.
