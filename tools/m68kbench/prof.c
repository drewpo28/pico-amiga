// tools/m68kbench: executed ARM instructions per address in 0x10000000 + 4 MB
// (the bench image), dumped at exit to $PROF_OUT; symprof.py folds them by
// symbol.  The counts are zeroed when the instruction at `mark=ADDR` runs
// (harness.c's bench_mark), so what comes before it is not measured.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "qemu-plugin.h"
QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
#define BASE 0x10000000u
#define SPAN (1u << 22)
// One scoreboard row (the single vCPU's): [0] the total, [1 + (addr - BASE) / 2].
static struct qemu_plugin_scoreboard *sb;
static uint64_t mark;
static uint64_t *row(void) { return qemu_plugin_scoreboard_find(sb, 0); }
static void on_mark(unsigned int cpu, void *u) { memset(row(), 0, (1 + SPAN / 2) * 8); }
static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, i);
        uint64_t va = qemu_plugin_insn_vaddr(in);
        qemu_plugin_u64 e = { sb, 0 };
        if (mark && va == mark) qemu_plugin_register_vcpu_insn_exec_cb(in, on_mark, QEMU_PLUGIN_CB_NO_REGS, NULL);
        qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(in, QEMU_PLUGIN_INLINE_ADD_U64, e, 1);
        if (va >= BASE && va < BASE + SPAN) {
            e.offset = (1 + ((va - BASE) >> 1)) * 8;
            qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(in, QEMU_PLUGIN_INLINE_ADD_U64, e, 1);
        }
    }
}
static void at_exit(qemu_plugin_id_t id, void *p) {
    const char *out = getenv("PROF_OUT");
    const uint64_t *c = row();
    if (out) {
        FILE *f = fopen(out, "w");
        for (uint32_t i = 0; i < SPAN / 2; i++) if (c[1 + i]) fprintf(f, "%08x %llu\n", BASE + i * 2, (unsigned long long)c[1 + i]);
        fclose(f);
    }
    fprintf(stderr, "INSNS %llu\n", (unsigned long long)c[0]);
}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv) {
    sb = qemu_plugin_scoreboard_new((1 + SPAN / 2) * 8);
    for (int i = 0; i < argc; i++) if (!strncmp(argv[i], "mark=", 5)) mark = strtoull(argv[i] + 5, NULL, 0) & ~1ull;
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
