// m68kdis: disassemble a 68000 binary with Musashi's disassembler.
//
//   m68kdis file.bin base_hex [from_hex [to_hex]]
//
// The file's first byte sits at base; from/to bound the listing.  tools/
// memdump.py runs it on the code around the PC of a board's memory dump.
#include <stdio.h>
#include <stdlib.h>
#include "m68k.h"

static unsigned char buf[1 << 20];
static unsigned base, n;

static unsigned rd8(unsigned a) { a -= base; return a < n ? buf[a] : 0; }
unsigned int m68k_read_disassembler_16(unsigned int a) { return rd8(a) << 8 | rd8(a + 1); }
unsigned int m68k_read_disassembler_32(unsigned int a) {
    return m68k_read_disassembler_16(a) << 16 | m68k_read_disassembler_16(a + 2);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s file.bin base_hex [from_hex [to_hex]]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    n = (unsigned)fread(buf, 1, sizeof buf, f);
    fclose(f);
    base = (unsigned)strtoul(argv[2], 0, 16);
    unsigned pc = argc > 3 ? (unsigned)strtoul(argv[3], 0, 16) : base;
    const unsigned end = argc > 4 ? (unsigned)strtoul(argv[4], 0, 16) : base + n;
    char s[256];
    while (pc < end) {
        const unsigned len = m68k_disassemble(s, pc, M68K_CPU_TYPE_68000);
        printf("%06X  %s\n", pc, s);
        pc += len ? len : 2;
    }
    return 0;
}
