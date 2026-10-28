# screenshot.gdb - the Amiga framebuffer as the scanout shows it (Ctrl+Alt+S and
# Ctrl+Alt+P), for pico-speccy's VS Code extension (local.picospec-tools): it
# halts the target, sources this file, resumes, runs tools/screenshot.sh and
# opens /tmp/pico-amiga_screen.png.  The file names are set in
# tools/picospec-tools.json; the extension checks they were written and waits
# for /tmp/pico-amiga_gdb_done.
#
#   /tmp/pico-amiga_fb.bin    main.c's fb[AMIGA_FB_H]: amiga_row_t rows (amiga.h)
#   /tmp/pico-amiga_dim.txt   "<sizeof(amiga_row_t)> <rows>"
shell rm -f /tmp/pico-amiga_gdb_done
set pagination off
# (a value dump would hit GDB's max-value-size: the rows are ~245 KB)
dump binary memory /tmp/pico-amiga_fb.bin &'main.c'::fb ((char *)&'main.c'::fb + sizeof('main.c'::fb))
set logging file /tmp/pico-amiga_dim.txt
set logging overwrite on
set logging redirect on
set logging enabled on
printf "%d %d\n", sizeof('main.c'::fb[0]), sizeof('main.c'::fb) / sizeof('main.c'::fb[0])
set logging enabled off
set logging redirect off
set logging overwrite off
printf "screenshot: dump done\n"
shell touch /tmp/pico-amiga_gdb_done
