<!-- Referenced from CLAUDE.md: details moved out of it on 2026-11-09, word for word. -->

# pico-amiga: the board, its video outputs and memory

## Host builds

- `build-host/pico-amiga` is the firmware itself on a PC (SDL2, like pico-speccy's
  host branch): `src/pico/main.c` + `input.cpp` built against `host/sdl/include`
  (Pico SDK stand-ins) and `host/sdl/*.cpp` (video/audio/input/psram over SDL,
  `host_fatfs.cpp` = the FatFs API over a folder, `--root`, default ~/pico-amiga,
  holding `amiga/kick.rom` etc. as on the card). Board-only code stays out of the
  files it compiles (`input_devices.cpp` is the board's, host_drivers.cpp the PC's).
  `PICO_AMIGA_SHOT=f.bmp:N` saves output frame N; `SDL_VIDEODRIVER=offscreen` runs it
  headless. `build-host/pico-amiga-host` remains the headless test bench.

## Board debugging

- Board debugging from VS Code (Cortex-Debug session, pico-speccy's local
  `picospec-tools` extension: it runs the open workspace's `tools/`, with the
  file names of `tools/pico-amiga-tools.json`): Ctrl+Alt+S / Ctrl+Alt+P =
  screenshot (`screenshot.gdb` dumps main.c's `fb`, `fb2png.py` expands it as
  `amiga_fb_line` does) to /tmp/pico-amiga_screen.png; Ctrl+Alt+M =
  `memdump.gdb` + `memdump.py`: /tmp/pico-amiga_dump.log (CPU, custom chips,
  code at PC via `build-host/m68kdis`, the copper list). All of chip / slow RAM
  only after `set $amiga_chip = 1` / `set $amiga_slow = 1` (seconds of halt).
  The two cores are separate GDB threads: a pause may stop core 1 alone, and a
  continue sent to core 0 left the HDMI core halted (black screen); the
  extension continues the threads that stopped.

## Drivers

- `src/pico/drivers/` are pico-speccy's drivers (USB HID/XInput, fabgl VK keyboard
  queue, PS/2, NES pad, SD); local changes are marked `PICO-AMIGA:`, and
  `drivers/input_glue.h` stands in for pico-speccy's Config/Debug/ESPectrum.
  `external/tinyusb` is pico-speccy's TinyUSB 0.21 copy (PICO_TINYUSB_PATH).
  Boards and pins: `cmake/boards.cmake` (same names and GPIOs as pico-speccy).

## Releases

- Releases (pico-speccy's scheme): `./build_all.sh` (MURM MURM2 PICO_PC PICO_DV
  ZERO2, Release, CPU_MHZ 252 = a fresh card's clock, build-all/<BOARD>, UF2s to firmware/),
  `./check-release.sh` (heap = __HeapLimit - end per image, tmds_test, smoke),
  `./draft-release.sh [ver] [--prerelease] [--skip-build] [--notes f]` (gh
  release with the Downloads table of tools/release_downloads.py). Claude
  commands: /pre-release, /draft-release, /release, /getch, /update-readme.

## Video outputs, PSRAM, timing

- **540 MHz** (Options > Overclock, 2026-11-09, pico-speccy's scheme): 25.2 MHz
  is 540 / 21.4, so video_modes.c has 27 MHz twins of the standard modes at
  clk_sys 540 (PIO divider 2.0, HSTX clk_hstx 135 / 5, VGA / 20; CEA's 864 x 625
  and 856 x 525 lines) and 36 MHz ones for 800x600; Config::load forces the core
  to 1.80 V with it (pico-speccy's DVp2 corrupted SRAM at 1.60-1.70 V); the PSRAM
  SCK is 540 / 4 = 135 MHz, the proven ceiling (psram.c); the flash limit's
  divider follows as always (66 -> 60 MHz).
- PIO TMDS (video_pio.c) goes out by lookup, pico-speccy's scheme with a
  palette per line (tmds_lut.h): a line is 416 entry numbers (one per two
  pixels), a DMA chain + a converter SM turn them into the entries' pin words.
  Core 1 builds lines two ahead, encoding only each new colour / hires pair of
  a line into its page (5 pages for 4 line buffers: a row's second line reuses
  the first's numbers). The entry window must be 64 KB-aligned: it is the
  `.hdmi_lut` section at ORIGIN(RAM), src/pico/ld/ overrides the SDK's
  section_ram_vector_table.incl. Entries come from tables of balanced character
  pairs (tmds_pio.h, tmds_pair.h); hires pairs of different colours use
  per-lane [16][16] tables whose second character may be off by up to 4 levels
  to keep each pair DC-balanced. The word format was checked bit-for-bit
  against pico-speccy's hdmi_pack3_pio / hdmi_pins_pio for the PICO_DV pin map;
  build-host/tmds_test decodes all 4096 colours like a receiver and checks the
  lookup lines against the CPU encoder word for word. Re-run it after touching
  tmds_pio.h, tmds_pair.h or tmds_lut.h.
- **Video > Output > Mode** (video_modes.c, pico-speccy's VM_* numbers and timings in
  storage.nvs hdmi_vmode / vga_vmode): 640x480 at 60 Hz (default: not every display
  takes 50 Hz; a 320 x 240 window on the standard screen; Picture position moves
  it; the menu's surface Sf is the window), 720x480 at 60 Hz (360 x 240: 720x576's
  line, 504 lines), 800x600 at 60 Hz (VM 17, ours: framed, 625 lines at 37.8 / 50.4 /
  33.6 MHz, HDMI only - VGA's 800x600 is 60 Hz already), 720x576 at 50 Hz, 800x600
  at 50 Hz (framed). UiDesc.cpp describes each (refresh, how much is cut). Each
  scanout has its rows: HSTX needs a pixel clock of clk_sys / 5, 10, 15 or 20
  (800x600 at 50 Hz: 37.8 MHz at 378, 50.4 at 252/504); the PIO HDMI a divider of a
  whole or half number (800x600 at 50 Hz: 37.8 at 378, 33.6 at 504, none at 252), its
  lookup line a runtime tl_layout_t (tmds_lut.h: black borders are TL_E_BLACK,
  tmds_test lays out every mode at every clock from video_modes.c and checks it); VGA runs 800x600 at VESA's 60 Hz (42 MHz, 1108 x
  628). A 50 Hz mode paces the emulation by its frames, 60 Hz by its lines in 20
  ms: a moving cut, as pico-speccy's V-Sync off. A new mode is kept only once
  confirmed: the commit writes the old ones to vmode_pending.nvs (F_VMODE) and the
  boot that finds it asks for 15 s, then reverts and reboots.
- **HSTX lines use no RAW_REPEAT / TMDS_REPEAT** (video_hstx.c): every control
  period is a RAW run, one word per pixel, as pico-speccy paces its blank lines; a
  black picture line is TMDS from a zero line. With RAW_REPEAT | 688 (640x480's
  blank-line back porch + width) the blank lines played ~300 px, the frame ran at
  63 Hz and no display locked (PCp2, MURM2: HSTX had never shown a picture); split
  into runs of 255 the stream fell apart. Lines/s per mode is the check.
- **No DMA reads from PSRAM while the PIO TMDS scanout runs** (PICO_DV): the DMA
  has one read master, a PSRAM read stalls it for hundreds of ns, the video
  channel starves and HDMI drops (tried: a per-line bitplane prefetch, hires
  screens lost the picture). XIP streaming would not stall it but bypasses the
  write-back XIP cache, so it reads stale chip RAM.
  Nor CPU copies through the uncached alias (2026-11-05, reverted 35682bb): a
  line per plane cleaned out of the cache and read uncached at line start cost
  ~45 ns a byte (4.6 ms a frame for 8 AGA planes), saved 4.5 ms of cached reads,
  and the cleans slowed the CPU: Roots' heaviest part went 31 -> 25 fps.
- Firmware: PSRAM init runs QMI direct mode — nothing may fetch from flash on
  either core meanwhile, so it runs before core 1 starts and before USB.
- **A frame starts when the scanout is 70% into its picture** (main.c,
  `video_lines()`): there is one framebuffer, and started at the top, a frame
  of 18-20 ms crossed the beam mid-screen (a moving cut). From 70%, Denise paints
  every row after the scanout showed it and before it shows it again.
- **Core 0's CPU time is chip RAM latency**, not Musashi's arithmetic: the PC
  sampler (matched ELF; pcprof.py names a 64-byte bucket by the function at its
  start, so "m68k_read_memory_8" is read_memory_16/32) put ~45% of Roots'
  CPU-bound seconds in the data reads, the fetch window's reads and the dispatch
  loop (the 128 KB handler index in flash), ~170 cycles an instruction; the
  bitplane reads (dma_unit, 20% of the render-bound seconds) are the same PSRAM
  behind 8-byte XIP lines.  Debug > PC profiler now samples core 1 too ("PROF1"
  lines, `tools/pcprof.py --core 1`; core 1's loop polls board_pcprof_core1_poll).
- **Check where AMIGA_HOT code lands** (`arm-none-eabi-nm`): a static AMIGA_HOT
  function inlined into a flash caller runs from flash. run_line sat inside
  amiga_run_frame that way (now noinline), the blits inside blitter.c's start().

## SRAM map (PICO_DV, 2026-11-09, `nm -S --size-sort`)

Of 520 KB: framebuffer `fb` 245 KB (288 rows x 852 B: mode, 64-colour palette,
720 B of pixels), the PIO HDMI entry window 34 KB, `audio_ring` 16 KB (130 ms;
the reader's target is 80 ms), the bitplane word ring `ws` 16 KB (25 lines of
lead at 8 planes), TMDS tables `tab_mix` 12 KB + `tab_gb` 4 KB, the TMDS map `tl`
8.6 KB, the event queue `evq` 8 KB, Denise's colour map 8 KB, Musashi's handler
table 7.9 KB + cycles 2 KB, `banks` 5 KB, `items` 4 KB, `pf_ring` 4 KB, core 1
stacks 6 KB; SRAM code 81 KB (378 handlers = 32 KB, the rest denise / video /
scheduler); heap ~25 KB.  What could be freed: the rows a 480-line mode never
shows (48 x 852 B = 41 KB, mode-dependent), `evq` at 512 events (4 KB), the
load test's scratch line (1 KB), 4 B per bank (1 KB).  The pico-speccy hot-page
pool (8 KB SRAM frames holding the guest's hottest PSRAM pages, every chip RAM
consumer going through one page resolver) needs 16-32 KB to matter, i.e. a
smaller framebuffer - which the owner ruled out (2026-11-09).

Freed instead (2026-11-09): Musashi's two hot handler sets share one SRAM window
as ld overlays (`src/pico/ld/section_extra_post_data.incl`, m68kconf.h
M68KI_HOT_SECTION; board_load_cpu_overlay() copies the running CPU's set from
flash at amiga_set_cpu; the heap starts at the window's end, HEAP_LOC): the
window is the larger set, 20.7 KB, not the sum of 32 KB, and the 68EC020's set
can grow to the window for free.  The audio ring (16 KB) moved to PSRAM at +2 MB
+ 32 KB (Paula writes it, the audio tick reads it, no DMA).  Heap on PICO_DV: 19
-> 50 KB.  A linker `.incl` change needs the ELF deleted: ninja does not track it.

Tried and reverted (2026-11-09, commits 4f97e37 / 83856a7): a fetch page pool -
3 x 8 KB of SRAM holding copies of the chip RAM pages the CPU runs from most,
the instruction fetch window pointing into them, every chip RAM writer writing
through, pages picked by PC samples once a frame.  Roots' zigzag part: the
CPU's samples were 92% on resident pages and XIP accesses fell 180k -> 150k a
frame, but XIP misses stayed at 12k a frame and `cpu` went 11.0 -> 12.0 ms: the
demo's code already lived in the 16 KB XIP cache, the misses are the CPU's data
and the DMA's, and the cost per instruction (~230 cycles) is Musashi's own
overhead.  A faster CPU needs that overhead cut, not a faster fetch.

## Measured

- Roots on the A1200 with the chip-bus model (2026-11-09 log, PROF1 = core 1):
  cpu 10-12 ms a frame (was 17-27).  The 8-plane hires parts (fps 36-38): core 0
  dma_unit 25% (rd 6-9 ms), core 1 tl_active 21%, spread_planes 11%, w_index +
  w_to_direct 8%, fill_bytes 7% (an outlined copy of the inline in flash, now
  AMIGA_HOT); the 8-plane lowres parts (fps 45): core 1 spread_planes + or32 +
  fetch_unit 31%, tl_active 14%, w_plain 9%.

- Host profile with AROS idling at its boot screen (hires, 4 planes): bitplane
  fetch/c2p ~50%, output ~20%, CPU ~10%. The renderer is the first thing to move
  to core 1 / optimize on the RP2350 (docs/PLAN.md, section 2).
- Firmware RAM (framebuffer 245 KB, colour map 8 KB, audio ring 16 KB, ~45 KB of SRAM code incl. 17 KB of Musashi handlers; DV adds TMDS
  tables/line buffers, of 520 KB): PICO_DV has ~8 KB between the end of .bss and
  __HeapLimit (2026-10-28, `nm` `end`); the old "~50 KB" was stale. That gap
  is the whole C/C++ heap (the menu's strings, the file browser): with ECS it
  fell to 5.4 KB and Devices > Floppy drives > DF0 panicked "Out of memory" on the board; the
  colour map's tag and index tables merged into one (-4 KB), the Win+F8
  band's pixels moved to PSRAM (16 KB at +2 MB, before the DMS area; a 2 MB
  PSRAM has no band) and the .m3u read through 256 bytes: 20.5 KB; System
  info's flash JEDEC read (hardware_flash) took 0.9 KB of it: 19.5 KB
  (2026-10-28); 13.9 KB by the 2026-10-30 HEAD before snapshots, 14.5 KB with
  them (their code is flash, their buffers the heap's while they run). Then
  VGA's 22 KB (pair LUT, templates) moved into the idle PIO HDMI entry window
  (`hdmi_spare()`: one scanout runs after boot) and the floppy's MFM track
  (12.4 KB, `amiga_config_t.track`) into PSRAM after the band (+2 MB + 16 KB; a
  2 MB PSRAM mallocs it), and the UART log's 4 KB ring is malloc'ed only when
  Debug > UART log is on: 52 KB on MURM / PICO_DV / ZERO2, 77 KB on MURM2 /
  PICO_PC (2026-10-30, ./check-release.sh). AGA (256 colours, LUTs, 8 plane pointers, the
  68EC020's handler set in flash) took it to 42 KB on MURM / PICO_DV (2026-11-05). Check `end` after adding SRAM code
  or tables.
- PICO_DV core 1 before the lookup scanout (Cannon Fodder, 504 MHz, ms per
  frame): CPU TMDS encoding 6.5, line IRQ 1.6, leaving Denise ~6 (the PERF
  line's `core1 render/irq/enc`).
