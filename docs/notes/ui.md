<!-- Referenced from CLAUDE.md: details moved out of it on 2026-11-09, word for word. -->

# pico-amiga: the menu, disks, machines and snapshots

## The menu (src/pico/ui/)

- `src/pico/ui/` is pico-speccy's nm:: menu (F12, WinUAE's key: F1..F10 are the
  Amiga's; Ctrl+F12 = DF0 browser, Shift+F12 or F11 = reset, Win+F11 = reboot the
  board, asked with Yes the default - pico-speccy's F12; Win+F1 = System info, the
  board/memory/temperature page, pico-speccy's Alt+F1 - Alt is the Amiga's; Win+F5
  = Open file, pico-speccy's F5 browser: Enter opens a disk / .m3u / Kickstart, F5
  on a disk picks DF0..DF3, F9 makes a blank ADF, F1 on a disk shows what is on it
  (machine_disk_info: volume, filesystem, boot block, root, startup-sequence; a
  DMS is unpacked into the spare PSRAM slot after DF3's); a disk put into DF1..DF3 by hand
  survives set and DF0 changes, Drive::manual; Win+F8 = the performance band,
  pico-speccy's F8, its summary with the drive's TR/SEC line - or the IDE hard
  disk's block, RD/WR and rate, whichever moved data last: amiga_ide_status). The drive icon
  (pico-speccy's FDD lamp, Video > Picture > Drive icon, top right; the hard disk's beside it) and the band
  (bottom) take the core's two overlay row ranges (amiga_set_overlay_rows(which,
  ..): one range spanning both would send every row through row_done). Its
  root follows pico-speccy's: Help, Machine, Devices (Floppy drives, Hard disk on a Gayle machine, Input), Video, ... The engine (UiNav, UiRender,
  UiDialog, UiFont, UiModel, the Stage mechanism) is kept close to pico-speccy, with
  `PICO-AMIGA:` notes; `UiGfx.cpp` draws 0x0RGB straight into the Amiga framebuffer,
  turning each row it touches into a DIRECT one (the scanout keeps showing it; the emulator is stopped while the menu runs, and its
  next frame repaints everything). The content is ours: `UiTree.cpp` (the tree),
  `UiStage.h` (the SettingId X-table, append only), `UiPlatform.*` (keys, reset,
  reboot), `app/Config.*` (settings in pico-speccy's storage.nvs format, at
  /.config/pico-amiga/<board tag>/storage.nvs). Check menu changes on the PC host:
  `PICO_AMIGA_KEYS=150:F12,200:Down,...` presses keys at output frames.

## Built-in ROMs, machines and drives

- Built-in ROMs (Machine: DiagROM; A500 > KS 1.2/1.3/2.04/2.05/3.1; A500+ > KS
  2.04/2.05/3.1; A600 > KS 2.05/3.1; A1200 > Kickstart KS 3.1/3.0 (ks31a12, ks30a12: a 68EC020,
  2 MB chip, Gayle $D1, AGA; Config::machine 3, `--a1200 --aga` on the bench; A1200 >
  Chip bus waits = amiga_set_chip_bus, Config::chip_bus);
  Custom ROM = a file from the card, Config::kick_file,
  Config::machine = 1: ECS, 1 MB chip RAM over the slow RAM's PSRAM, 2: the same plus
  Gayle (machine.h MACHINE_*), Machine > A600 chip RAM 1 or 2 MB (an A601, Config::a600_chip;
  with 8 MB of PSRAM - every board has it - the chip RAM area is 2 MB and the ROM buffers,
  the Win+F8 band, the MFM track and the DMS area sit 1 MB higher, main.c), `amiga_set_machine()`;
  the bench: `--a500plus`, `--a600`, `--a601` (2 MB chip);
  the menu's SET_KICK value is the ROM index + 200 x Config::machine) are linked
  into the firmware and the host with `.incbin` from `ROMS_DIR` (default `debug/`,
  git-ignored: the Kickstarts are Commodore's and must never be committed).
  `cmake/roms.cmake` lists them; an id there is what storage.nvs keeps, never
  reuse one. The firmware targets 16 MB flash; the core reads the ROM in place
  (XIP). `machine.cpp` owns ROM selection and the drives: DF0..DF3 (Devices > Floppy drives >
  Drives, Auto = one per disk of an `.m3u` set, or its `#DRIVES:n` line - ours:
  Gobliiins reads DF0 only and hangs short of memory with 3; Kickstart finds DF1+ by drive ID
  at reset). An ADF streams from the card; a `.dms` is unpacked at insert time by
  `src/pico/dms.c` (xDMS port, its ~100 KB work area in PSRAM) into a per-drive
  880 KB PSRAM slot above 2 MB, write-protected. The bench takes `--adf x.dms` too,
  up to 8 `--adf` (DF0..DF3, the rest for `--swap`), `--drives N` and `--write-back`
  (written disks saved to their files). Devices > Floppy drives > New blank disk formats an empty
  OFS ADF on the card (`machine_new_disk`); with an .m3u set loaded it is appended
  to the set (games' own save-disk formatters: swap it in with Ctrl+F12).

## Snapshots

- Snapshots (pico-speccy's `.pss` as `.pas`): `src/amiga/state.c` saves and
  loads the core between frames as blocks (`amiga_state_save/load`); each module's
  `*_state(st_t*)` moves its fields both ways (ST_SIZE / ST_SAVE / ST_LOAD), fixes
  up what derives from them under ST_LOAD, and takes a new field at its END (older
  snapshots read it as 0). MACH comes first (model, ROM CRCs) and is checked before
  anything changes. `src/pico/snapshot.cpp` is the file: "PAS1" header, NAME
  (renamed in place), CFG (model, Kickstart, the drives' disks with size and date),
  then the core's blocks; the disks' contents are not in it. Snapshots > Load from
  file / Quick slots (40, `/amiga/snapshots/persistN.pas`, `ui/UiSnap.cpp`);
  Win+F3 / Win+F4 open the slots (Enter loads / saves), Alt+Win+F3 / Alt+Win+F4
  load / save `Config::persist_slot` at once; a `.pas` opens from Win+F5 (no F2
  key). The bench: `--state-save N:file` / `--state-load file` (the core's blocks
  only); smoke checks a round trip mid-boot.

## LHA archives and WHDLoad

- An `.lha` / `.lzh` opened on the A600 / A1200 (Win+F5, or Devices > Hard disk >
  Master > Mount... > Image file) boots as the IDE master (`machine_boot_lha`): `src/pico/lha.c`
  unpacks it (lh0, lh4-lh7, header levels 0-2), `ffs.c` writes a bare DOS\1
  partition (ide.c's virtual RDB goes in front), `whdboot.c` decides what goes on
  it - the archive, `C/WHDLoad`, the Kickstart images the slave names (a
  case-insensitive search of the slave for "40068.A1200" etc.; UFO CD32 lists three)
  from the built-in ROMs under their WHDLoad names (`builtin_rom_t.file`) with
  their `.RTB`, the files of the card's `/amiga/whdload/` (first wins), and
  `S/Startup-Sequence`: `CD` into the slave's folder, `C:WHDLoad SLAVE=x PRELOAD`
  plus the icon's tooltypes (the icon whose `SLAVE=` names a slave; WHDLoad's own
  local options only, `$hex` made decimal as its CLI wants, MMU / Snoop / Chk left
  out). No slave: the first hunk file near the top runs instead; an archive with
  its own `S/Startup-Sequence` keeps it. A bare executable (Win+F5: any file that
  starts with $000003F3, `machine_is_program`) gets a volume of its own the same way
  (`whd_job_t.run`, no archive). Every volume has empty C, S, L, Libs, Devs, Fonts:
  the boot assigns only folders that exist, and a missing LIBS: asks for a volume.
- The image is `/amiga/whdload/images/<name>.hdf`, built once (the DMS area is the
  arena: the floppies are ejected - DF0 would boot first - and the build runs on
  an 8 KB heap stack, pico-speccy's zipCallOnStack) and stamped at byte 16 of block 0
  with the archive's size and date: an unchanged archive boots its image again
  with the saves in it. 2 MB are left free for them.
- WHDLoad's files are linked from `ROMS_DIR/WHDLoad/` (cmake/roms.cmake: every file
  there, its path there its path on the volume); none: the card folder must have
  `C/WHDLoad`. WHDLoad 20.1 runs slaves on the 68000 with KS 2.05 / 3.1.
- Machine > A600 / A1200 > Fast RAM (Config::fast_ram: 0, 1, 2, 4 MB): a Zorro II
  board from the end of the DMS area (machine_fast_max keeps the builder's arena;
  drives whose DMS slot it takes are ejected). WHDLoad's ExpMem ("other memory")
  needs it: UFO CD32 asks for 1.5 MB ("Can't allocate ExpMem" without).
- Checked: HeroQuest (A600 KS 2.05 / 3.1, A1200), UFO Enemy Unknown CD32 (A1200 + 4 MB);
  install-only archives (HATE.lha: the installer and slave, no data) stop in
  WHDLoad with "object not found", as on a real Amiga.
- Devices > Hard disk (A600 / A1200): Master and Slave (`machine_set_hdf(unit, ..)`,
  Config::hdf / hdf_raw and hdf2 / hdf2_raw). Mount... is one list, pico-speccy's
  rawSourcePick: "Image file (HDF, LHA)..." then the card's partitions (the one in
  use under the cursor; no partition table: straight to the browser). An LHA goes
  to the master only (it is the boot drive; the slave holding that image lets go
  of it); one file or one writable window never sits in both drives.
