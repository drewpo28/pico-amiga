# screenshot_profi.gdb - Ctrl+Alt+P in pico-speccy's VS Code extension (its
# "Profi" screenshot): the same capture as screenshot.gdb under the file names
# that binding expects (GDB runs in OpenOCD's folder, so this file cannot source
# the other one by a relative path).
shell rm -f /tmp/picospec_gdb_done
set pagination off
# (a value dump would hit GDB's max-value-size: the rows are ~245 KB)
dump binary memory /tmp/picospec_profi_fb.bin &'main.c'::fb ((char *)&'main.c'::fb + sizeof('main.c'::fb))
dump binary value /tmp/picospec_profi_pal.bin 'denise.c'::color
dump binary value /tmp/picospec_profi_lut.bin 'denise.c'::color
dump binary value /tmp/picospec_pal.bin 'denise.c'::color
set logging file /tmp/picospec_profi_dim.txt
set logging overwrite on
set logging redirect on
set logging enabled on
printf "%d %d\n", sizeof('main.c'::fb[0]), sizeof('main.c'::fb) / sizeof('main.c'::fb[0])
set logging enabled off
set logging redirect off
shell echo amiga > /tmp/picospec_mode.txt
printf "screenshot: dump done\n"
shell touch /tmp/picospec_gdb_done
