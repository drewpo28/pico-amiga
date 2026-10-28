# memdump.gdb - the Amiga's memory and chip state (Ctrl+Alt+M), for pico-speccy's
# VS Code extension (local.picospec-tools): it halts the target, sources this
# file, resumes, runs tools/memdump.py and opens /tmp/pico-amiga_dump.log
# (tools/pico-amiga-tools.json names the files).
#
#   /tmp/pico-amiga_pc.bin     256 bytes from PC-64 (chip RAM or Kickstart ROM)
#   /tmp/pico-amiga_cop.bin    2 KB of chip RAM from COP1LC (the copper list)
#   /tmp/pico-amiga_chip.bin   all of chip RAM, only after `set $amiga_chip = 1`
#   /tmp/pico-amiga_slow.bin   slow RAM, only after `set $amiga_slow = 1`
#                             (both in the Debug Console before Ctrl+Alt+M)
# The whole RAM is opt-in: it keeps the target halted for seconds (512 KB of
# PSRAM through SWD each); the default dump is about as short as a screenshot.
#   /tmp/pico-amiga_state.txt  CPU registers, custom chips (memdump.py reads the
#                             "KEY value" lines, the rest goes to the log as is)
# Every symbol below exists in the firmware; a missing one would stop the script
# half-way (the extension then reports the files that were not written).
# GDB's print and logging settings are put back at the end: Cortex-Debug keeps
# using this GDB after the script.
shell rm -f /tmp/pico-amiga_gdb_done /tmp/pico-amiga_slow.bin /tmp/pico-amiga_chip.bin /tmp/pico-amiga_cop.bin
set pagination off

if $_isvoid($amiga_chip)
  set $amiga_chip = 0
end
if $_isvoid($amiga_slow)
  set $amiga_slow = 0
end
if $amiga_chip
  dump binary memory /tmp/pico-amiga_chip.bin A.cfg.chip_ram (A.cfg.chip_ram + A.cfg.chip_size)
end
set $q = A.cfg.chip_ram + ('copper.c'::lc[0] & A.chip_mask & ~1)
if ('copper.c'::lc[0] & A.chip_mask) + 2048 <= A.cfg.chip_size
  dump binary memory /tmp/pico-amiga_cop.bin $q ($q + 2048)
end
if $amiga_slow && A.cfg.slow_ram != 0
  dump binary memory /tmp/pico-amiga_slow.bin A.cfg.slow_ram (A.cfg.slow_ram + A.cfg.slow_size)
end
set $apc = m68ki_cpu.pc & 0xFFFFFF
if $apc >= 0xF80000
  set $p = A.cfg.rom + (($apc - 64) & (A.cfg.rom_size - 1))
else
  set $p = A.cfg.chip_ram + (($apc - 64) & A.chip_mask)
end
dump binary memory /tmp/pico-amiga_pc.bin $p ($p + 256)

set logging file /tmp/pico-amiga_state.txt
set logging overwrite on
set logging redirect on
set logging enabled on
printf "PC %06X\nPPC %06X\n", m68ki_cpu.pc, m68ki_cpu.ppc
printf "D %08X %08X %08X %08X %08X %08X %08X %08X\n", m68ki_cpu.dar[0], m68ki_cpu.dar[1], m68ki_cpu.dar[2], m68ki_cpu.dar[3], m68ki_cpu.dar[4], m68ki_cpu.dar[5], m68ki_cpu.dar[6], m68ki_cpu.dar[7]
printf "A %08X %08X %08X %08X %08X %08X %08X %08X\n", m68ki_cpu.dar[8], m68ki_cpu.dar[9], m68ki_cpu.dar[10], m68ki_cpu.dar[11], m68ki_cpu.dar[12], m68ki_cpu.dar[13], m68ki_cpu.dar[14], m68ki_cpu.dar[15]
printf "USP %08X\nSSP %08X\n", m68ki_cpu.sp[0], m68ki_cpu.sp[4]
printf "FLAGS t1 %X s %X x %X n %X notz %X v %X c %X int %X\n", m68ki_cpu.t1_flag, m68ki_cpu.s_flag, m68ki_cpu.x_flag, m68ki_cpu.n_flag, m68ki_cpu.not_z_flag, m68ki_cpu.v_flag, m68ki_cpu.c_flag, m68ki_cpu.int_mask
printf "BEAM frame %u vpos %u lines %u lof %u\n", A.frame, A.vpos, A.lines, A.lof
printf "DMACON %04X\nINTENA %04X\nINTREQ %04X\nADKCON %04X\nCOPCON %04X\n", A.dmacon, A.intena, A.intreq, A.adkcon, A.copcon
printf "CHIP %u\nSLOW %u\nROM %u\n", A.cfg.chip_size, A.cfg.slow_size, A.cfg.rom_size
printf "COP1LC %06X\nCOP2LC %06X\nCOPPC %06X\nCOPSTATE %d\nCOPH %d\n", 'copper.c'::lc[0], 'copper.c'::lc[1], 'copper.c'::pc, 'copper.c'::state, 'copper.c'::cop_h
printf "BPLCON0 %04X\nBPLCON2 %04X\nDIWSTRT %04X\nDIWSTOP %04X\n", 'denise.c'::bplcon0, 'denise.c'::bplcon2, 'denise.c'::diwstrt, 'denise.c'::diwstop
printf "DDF %02X %02X BPLCON1 %04X\n", 'denise.c'::dma_st.ddfstrt, 'denise.c'::dma_st.ddfstop, 'denise.c'::dma_st.bplcon1
printf "BPLPT %06X %06X %06X %06X %06X %06X\n", 'denise.c'::bplpt[0], 'denise.c'::bplpt[1], 'denise.c'::bplpt[2], 'denise.c'::bplpt[3], 'denise.c'::bplpt[4], 'denise.c'::bplpt[5]
printf "BPLMOD %d %d\n", 'denise.c'::bplmod[0], 'denise.c'::bplmod[1]
printf "SPRPT %06X %06X %06X %06X %06X %06X %06X %06X\n", 'denise.c'::sprpt[0], 'denise.c'::sprpt[1], 'denise.c'::sprpt[2], 'denise.c'::sprpt[3], 'denise.c'::sprpt[4], 'denise.c'::sprpt[5], 'denise.c'::sprpt[6], 'denise.c'::sprpt[7]
printf "COLOR"
set $i = 0
while $i < 32
  printf " %03X", 'denise.c'::color[$i]
  set $i = $i + 1
end
printf "\n"
printf "BLIT con0 %04X con1 %04X afwm %04X alwm %04X size %04X busy %d count %u\n", 'blitter.c'::con0, 'blitter.c'::con1, 'blitter.c'::afwm, 'blitter.c'::alwm, 'blitter.c'::size, 'blitter.c'::busy, 'blitter.c'::nblits
printf "BLITPT A %06X B %06X C %06X D %06X mods %d %d %d %d\n", 'blitter.c'::pt[0], 'blitter.c'::pt[1], 'blitter.c'::pt[2], 'blitter.c'::pt[3], 'blitter.c'::mod[0], 'blitter.c'::mod[1], 'blitter.c'::mod[2], 'blitter.c'::mod[3]
printf "DISK sel %d side %d cyl0 %d dsklen %04X dskpt %06X dma %d track %d\n", 'floppy.c'::sel, 'floppy.c'::side, 'floppy.c'::drv[0].cyl, 'floppy.c'::dsklen, 'floppy.c'::dskpt, 'floppy.c'::dma_on, 'floppy.c'::trk_num
printf "--- CIAs\n"
set print pretty on
print/x 'cia.c'::cia
set print pretty off
set logging enabled off
set logging redirect off
set logging overwrite off
printf "memdump: done\n"
shell touch /tmp/pico-amiga_gdb_done
