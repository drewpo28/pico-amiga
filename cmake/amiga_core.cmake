# The emulation core (src/amiga + Musashi) as a static library, shared by the
# firmware and the host build.  ROOT must point at the repository root.
set(AMIGA_CORE_SRC
    ${ROOT}/src/amiga/amiga.c
    ${ROOT}/src/amiga/blitter.c
    ${ROOT}/src/amiga/cia.c
    ${ROOT}/src/amiga/copper.c
    ${ROOT}/src/amiga/custom.c
    ${ROOT}/src/amiga/denise.c
    ${ROOT}/src/amiga/fb.c
    ${ROOT}/src/amiga/floppy.c
    ${ROOT}/src/amiga/gayle.c
    ${ROOT}/src/amiga/ide.c
    ${ROOT}/src/amiga/idle.c
    ${ROOT}/src/amiga/mem.c
    ${ROOT}/src/amiga/paula.c
    ${ROOT}/src/amiga/rtc.c
    ${ROOT}/src/amiga/state.c
    ${ROOT}/external/musashi/m68kcpu.c
    ${ROOT}/external/musashi/gen/m68kops.c
    ${ROOT}/external/musashi/gen/m68kops_020.c
    ${ROOT}/external/musashi/softfloat/softfloat.c
)
add_library(amiga_core STATIC ${AMIGA_CORE_SRC})
target_include_directories(amiga_core PUBLIC
    ${ROOT}/src/amiga
    ${ROOT}/external/musashi
    ${ROOT}/external/musashi/gen
)
# Musashi is third-party code: keep its warnings out of ours.
set_source_files_properties(
    ${ROOT}/external/musashi/m68kcpu.c
    ${ROOT}/external/musashi/gen/m68kops.c
    ${ROOT}/external/musashi/gen/m68kops_020.c
    ${ROOT}/external/musashi/softfloat/softfloat.c
    PROPERTIES COMPILE_OPTIONS "-w")
target_compile_options(amiga_core PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
# Musashi's instruction fetch reads mem.c's window directly (m68kcpu.h); without
# it (Musashi's own test driver) the stock m68k_read_memory_16/32 path.
target_compile_definitions(amiga_core PRIVATE AMIGA_FETCH_WINDOW=1)
