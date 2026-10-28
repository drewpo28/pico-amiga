# screenshot.gdb - the Amiga framebuffer as the scanout shows it (Ctrl+Alt+S, and
# Ctrl+Alt+P through screenshot_profi.gdb), for pico-speccy's VS Code extension
# (local.picospec-tools): it halts the target, sources this file, resumes and
# runs tools/screenshot.sh.  The extension checks that the files below were
# written and waits for /tmp/picospec_gdb_done.
#
#   /tmp/picospec_fb.bin    main.c's fb[AMIGA_FB_H]: amiga_row_t rows (amiga.h)
#   /tmp/picospec_dim.txt   "<sizeof(amiga_row_t)> <rows>"
#   /tmp/picospec_pal.bin   Denise's colour registers (for reference)
#   /tmp/picospec_mode.txt  "amiga"
shell rm -f /tmp/picospec_gdb_done
set pagination off
# (a value dump would hit GDB's max-value-size: the rows are ~245 KB)
dump binary memory /tmp/picospec_fb.bin &'main.c'::fb ((char *)&'main.c'::fb + sizeof('main.c'::fb))
dump binary value /tmp/picospec_pal.bin 'denise.c'::color
set logging file /tmp/picospec_dim.txt
set logging overwrite on
set logging redirect on
set logging enabled on
printf "%d %d\n", sizeof('main.c'::fb[0]), sizeof('main.c'::fb) / sizeof('main.c'::fb[0])
set logging enabled off
set logging redirect off
shell echo amiga > /tmp/picospec_mode.txt
printf "screenshot: dump done\n"
shell touch /tmp/picospec_gdb_done
