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
- [ ] A600 profile: section 6 (before the A1200, which reuses its Gayle)
- [x] PIO TMDS back-end (PICO_DV); MURM1 / ZERO2 board entries still to add; VGA
- [ ] Video > Output > Mode (pico-speccy's modes, video_modes.c): 720x576@50, 800x600
      framed (HDMI 50 Hz, VGA VESA 60 Hz), 640x480@60 cropped, with the
      keep-or-revert boot question, on HSTX, PIO HDMI and VGA [x, host + tmds_test];
      hw: each mode on MURM2 (HSTX), PICO_DV (PIO, 378 and 504), a VGA board.
      Later: 1440x576@50 (VIC 29) for super-hires 1:1 (a 1440-pixel row format, 54 MHz = clk_sys 540)
- [ ] HDMI audio, I2S DAC
- [ ] Save states
- [ ] LHA archives (later, with WHDLoad below): most .lha games on the net are
      WHDLoad packages. A plain one (e.g. ArmyMiner-1.2.lha: one 127 KB program,
      its .info, doc and high scores, lh5) could go in like a .dms: an lh0/5/6/7
      decoder (src/pico/lha.c, level 0-2 headers; lhasa is ISC) into an OFS
      image in a PSRAM drive slot (~830 KB of files at most), either a data
      disk for DF1 next to Workbench or bootable with a startup-sequence
      that runs it (then the program has none of Workbench's libraries)
- [ ] WHDLoad: needs a hard disk (an autoconfig board, a boot ROM and a
      filesystem handler over a card folder or an HDF), WHDLoad itself, the
      Kickstart files it wants, and fast RAM the A500 profile does not have yet

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

## 6. A600 (ECS + Gayle)

An A600 is an A500+ with Gayle: 68000 at 7.09 MHz, 1 MB of chip RAM and no slow
RAM, ECS (Agnus 8375, Denise 8373), Kickstart 2.05 (37.350 is the built-in ks205,
kick37350.A600; 37.299 / 37.300 come as Custom ROM), an IDE port and a PCMCIA slot,
no battery clock. It comes before the A1200 and apart from it: everything but
Gayle is there already (section 5), and Gayle is checked here on the known 68000 /
ECS core, so the A1200's step 2 only reuses it. Config::machine = 2, `--a600` on
the bench (the A1200 becomes 3).

Step 1 - the machine (host):

- [x] cmake/roms.cmake: ks205 and ks31 listed under the A600 too; Machine > A600
      row; Custom ROM on it (Config::machine = 2, machine.h MACHINE_A600)
- [x] Memory map (WinUAE's for IDE_A600A1200 + PCMCIA): 1 MB chip as the A500+,
      CIAs at $BF0000 only, nothing at $A00000-$BEFFFF or $C00000-$D7FFFF, no
      clock at $DC0000
- [x] Gayle (gayle.c): the ID at $DD/$DE1000, one bit per byte read in bit 7, a
      write restarts it ($D0: KS 2.05 reads four bits, 1101); the IDE at $DA0000
      answers "no drive" ($7F status, $FF elsewhere): KS 2.05 gives up in the
      same frame; CS / IRQ / INT / CFG at $DA8000-$DAB000 as WinUAE keeps them
- [x] state.c: Gayle at the end of MACH (checked on load), its registers in GAYL
- [x] Check: KS 2.05 boots to the insert-disk screen as fast as on the A500+;
      smoke.sh test 6 (make_gayle_adf.py with AROS): ID, IDE status, no CIA mirror,
      the A500+ without them, a snapshot round trip on the A600
- [ ] Workbench 2.05 from ADF reports 1 MB chip; DiagROM on the A600
- [ ] The A600's floppy: one internal drive (DF1+ external), nothing to change
      unless a title cares

Step 2 - IDE with HDF images (host, shared with the A1200 and WHDLoad):

- [x] An ATA drive behind Gayle (ide.c, WinUAE's ide.cpp): IDENTIFY, READ/WRITE
      SECTORS (PIO, LBA28 and CHS), INITIALIZE DEVICE PARAMETERS, the interrupt
      through Gayle's IRQ/INT to level 2; a sector at a time through 512 bytes.
      An HDF with its RDB, or a bare partition behind a virtual RDB (RDSK + PART,
      WinUAE's 32 x 1 geometry, 2 reserved); the bench's --hdf (--write-back)
- [x] Devices > Disks > Hard disk (A600 only): HDF file... (fast seek), or SD
      partition... - pico-speccy's raw IDE window: the card's own sectors, its MBR
      read by ide_mbr.h (5-8: the logical ones), read-only or writable, never
      writable over the card's FAT volume; Config::hdf / hdf_raw, snapshots' CFG.
      The PC firmware's --sd card.img (host_disk.cpp) stands in for the raw card
- [x] Check: Workbench 2.04 on KS 2.05 and 3.1 on KS 3.1 mount a bare and an RDB
      HDF (make_hdf.py), Workbench 3.1 writes to it (makedir); smoke.sh test 7
      (make_ide_adf.py): IDENTIFY, LBA/CHS, two sectors, a write, the interrupt
- [ ] A Workbench installed on an HDF booting from it; a PFS3 RDB (filesystem
      loaded from the RDB); a > 504 MB image (CHS limits of scsi.device)
- [ ] SD2: browsing the card's other FAT partitions (pico-speccy's SdVolumes:
      a local FatFs patch for logical partitions) - the files, not the IDE window
- [ ] Writes: an f_sync per sector on HDF files (as the floppies); measure on the board
- [x] Machine > A600 chip RAM: 2 MB (an A601), the PSRAM layout's chip area 2 MB on 8 MB
      boards; the bench's --a601. A 2 GB FFS partition validates with it, not with 1 MB
- [ ] The A601's clock at $DC0000 with 2 MB (the firmware has no time source yet);
      PCMCIA fast RAM (4 MB at $600000, card.resource's CIS) as the other expansion

Step 3 - hw: the A600 on a board (heap / `end` with ./check-release.sh).

## 7. A1200 (AGA)

Nothing of it is there yet: the CPU is a 68000 (m68kconf.h has EC020/020 off,
amiga.c sets M68K_CPU_TYPE_68000), Denise knows OCS/ECS only (32 colour
registers, 6 bitplane pointers, a 64-entry LUT) and the frame buffer is 12-bit.
An A1200 is a 68EC020 at 14.18 MHz (twice the A500's clock, 24-bit addresses),
2 MB of chip RAM on a 32-bit bus, no slow RAM, Alice (AGA Agnus) + Lisa (AGA
Denise), Gayle (IDE, PCMCIA) and Kickstart 3.1 A1200 (40.068, 512 KB; 3.0 is
39.106). It is a fourth machine (Config::machine = 3, `--a1200` on the bench),
not AGA on an A500: no real A500 had it, and the ROMs differ.

Scope: PAL, 12-bit output. AGA's 24-bit colours are cut to their top 4 bits
per gun (every scanout, the TMDS lookups and the menu are 0x0RGB; HAM8's
262,144 colours become 4,096). A wider output is a separate, later project.

Step 0 - is there room for it (hw, before anything else):

- [ ] (switch ready: Debug > A1200 load test, the bench's --load-test 1/2; amiga_set_load_test:
      the 68000 runs twice the cycles per line - KS 1.3 reaches frame 100's PC at 50 -
      and two more planes are read and spread into a scratch line, an upper bound:
      AROS hires 4 planes went 79 -> 183 us of fetch, 38 -> 77 of reads on the host)
      Measure the headroom on the board: the A500 core with the CPU given twice
      the cycles per line (a debug switch) on Cannon Fodder / State of the Art,
      ms per frame at 378 and 504 MHz. Today a busy frame takes 18-20 ms; if
      the doubled CPU alone does not fit in 20 ms, AGA runs below full speed
      (frame skip or a slower emulated clock) and the steps below must say so
- [ ] The same with 8-plane fetch + c2p simulated (6 planes is today's cost):
      the renderer is already the larger half on core 1

Step 1 - the CPU: 68EC020 (host):

- [ ] m68kconf.h: M68K_EMULATE_EC020 on (020/040 stay off); re-run
      tools/gen_musashi.py. New handlers: bit fields, 32-bit MULx/DIVx.L,
      CAS/CHK2/CMP2, PACK/UNPK, EXTB, scaled index and memory-indirect EA
      modes, MOVEC (VBR, CACR, CAAR, MSP/ISP), RTD, TRAPcc, BKPT
- [ ] The 68000 machines must not get slower: check that the runtime
      CPU_TYPE_IS_* tests stay out of the hot handlers (generate two handler
      sets if they do not), and that A500 screens and smoke stay unchanged
- [ ] tools/musashi_hot.txt: a second profile from an AGA game; the SRAM set
      stays at ~17 KB (PICO_DV's heap is the limit), so the two lists share it
- [ ] Per-handler cycles: Musashi's 020 table; cycles scale x2 against the
      colour clock (the scheduler counts CPU cycles per line). The instruction
      cache (CACR) is not modelled beyond storing CACR; chip-bus contention
      stays unmodelled as on the A500 (section 3)
- [ ] Re-check with Musashi's test suite built for 68020 (test/mc68020, if the
      tree has it) and state.c: the new registers (VBR, CACR, CAAR, MSP) at the
      end of the CPU block

Step 2 - the machine (host):

- [ ] cmake/roms.cmake: A1200 machine, KS 3.1 A1200 (and 3.0) with new ids;
      Machine > A1200 row; Custom ROM on it; DiagROM checks the A1200 too
- [ ] Memory map: 2 MB chip RAM (DMA pointers are already 21 bits), nothing at
      $C00000 (Kickstart must not find slow RAM), chip_mask 2 MB
- [ ] PSRAM layout (main.c): chip RAM 0-2 MB pushes the Custom ROM buffers, the
      band, the MFM track and the DMS area up by 1 MB; an A1200 needs at least
      a 4 MB PSRAM (2 MB boards: the row is disabled, as the band is today)
- [ ] VPOSR Alice ID (PAL $22 / $23, check against WinUAE), DENISEID Lisa ($F8)
- [ ] Gayle: from the A600 (section 6, its ID and "no drive" IDE, then the HDF
      drive); check the A1200's Gayle ID and register differences against WinUAE.
      PCMCIA not fitted
- [ ] Optional fast RAM (none on a stock A1200; WHDLoad and many games want
      it): 4 MB at $200000 from PSRAM, on boards with 8 MB
- [ ] Check: KS 3.1 boots to the insert-disk screen, Workbench 3.1 from ADF
      reports 2 MB chip; AROS (free) for smoke

Step 3 - Alice: AGA bitplane and sprite DMA (host):

- [ ] FMODE ($1FC): bitplane fetch 1x / 2x / 4x (16 / 32 / 64 bits). A fetch
      unit stays 8 cc but carries 1, 2 or 4 words per plane; DDFSTRT/DDFSTOP
      round to the wider unit; modulos are added after the wider fetch (games
      align to 64 bits). dma_* in denise.c reads 2 or 4 words per slot into ws[]
- [ ] 8 bitplanes: bplpt[8], BPLCON0 BPU3 (bit 4), 7 and 8 planes in every
      resolution. ws[] (16 KB) holds fewer lines of lead at 8 planes x 4x: check
      the 40-80 line lead still holds, grow the ring if not (SRAM)
- [ ] Sprites: FMODE SPR32/SPR64 (16 / 32 / 64 pixels wide, 2 or 4 words per
      fetch, control words in the wide format), SSCAN2 (sprite scan doubling)
- [ ] BSCAN2 and the second modulo pair (BPL1MOD/BPL2MOD per line parity):
      productivity / scan-doubled modes, low priority for PAL games

Step 4 - Lisa: the picture (host):

- [ ] 256 colour registers: BPLCON3 BANK (bits 13-15) picks 32 of them,
      LOCT (bit 9) picks the low nibbles; a write with LOCT=0 sets both halves,
      as on the chip. We keep 24 bits per register (for snapshots and a
      later wider output) but render the top 12 bits, so a LOCT=1 write changes nothing on screen and is dropped before
      the queue (keeps copper rainbows that write both halves cheap)
- [ ] lut[] / lut_pf[] grow from 64 to 256 entries (+~400 B); the pf value is
      already a byte per pixel. The c2p for 7-8 planes (bytewise tables or the
      SIO interpolators, section 2) - 8 planes is a third more work than 6
- [ ] BPLCON4: BPLAM (XOR on the bitplane colour index), ESPRM / OSPRM (even /
      odd sprite colour banks); BPLCON3 PF2OF (dual playfield 2's colour
      offset), SPRES (sprite resolution), BRDRBLNK / BRDNTRAN / BRDSPRT
- [ ] HAM8 (6 planes of data, 2 control bits: modify the top 6 bits of a gun)
      and EHB stays for 6-plane non-AGA modes. HAM6 unchanged
- [ ] Sprite colours from the selected bank, sprite pixels at SPRES, the 32/64
      wide shift registers; CLXCON2 (collision of planes 7-8)
- [ ] Frame buffer rows: lowres rows are DIRECT (360 x 0x0RGB) and take any
      number of colours already. Hires rows are IDX8 with a 64-colour row
      palette: a hires AGA line with more colours converts to DIRECT
      (w_to_direct, pairs averaged as super-hires does). A 256-entry row
      palette for every row would cost 288 x 384 B = 110 KB of SRAM - not
      possible; a DIRECT 720-pixel row (1440 B) neither. Count how often games
      hit it before considering more
- [ ] LACE4 (16 colours per field line) falls back the same way (section 3)
- [ ] State: the 256 x 24-bit palette, BPLCON4, FMODE, bplpt[6..7] at the end
      of the Denise block

Step 5 - checks and the board:

- [ ] Reference screens (docs/screens/): KS 3.1 boot, Workbench 3.1 in
      256-colour lowres and hires, AROS AGA; a set of AGA games from ADF
      (owner's: HAM8 titles, 8-plane games, 64-pixel sprites, copper bank
      rainbows) checked against WinUAE, sync vs `--render-thread`
- [ ] smoke.sh: an A1200 test with AROS (no Commodore ROM in the repo)
- [ ] hw: on a board with >= 4 MB PSRAM; `end` / heap after the larger handler
      set and LUTs (./check-release.sh), ms per frame with the PERF band
