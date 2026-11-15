/*
 * TCG plugin: per-window instruction cost for emu/voicecost.py --worst.
 * costs=FILE: lines "addr cost fn" (hex addr, weighted cycles, function
 * index). mark=ADDR: a function whose r0 opens (>= 0, the window's label)
 * or closes (< 0) a window. out=FILE: per window "label total" and, for the
 * costliest windows (from=N: of the windows labelled N or more), "fn
 * cycles" lines, and "S fn cycles" summed over all windows from N on.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAXFN 8192
#define KEEP 8

struct item { uint32_t fn, cost; };
struct tbc { unsigned n; struct item it[]; };
struct win { int32_t label; uint64_t total; uint64_t fn[MAXFN]; };

static GHashTable *table;
static uint64_t mark_addr;
static const char *out_path;
static struct qemu_plugin_register *r0;
static int open_label = -1, from_label;
static uint64_t cur_total, cur_fn[MAXFN], sum_fn[MAXFN];
static GArray *labels, *totals;
static struct win *top[KEEP];

static void on_exec(unsigned int vcpu, void *udata)
{
    struct tbc *t = udata;

    if (open_label < 0) {
        return;
    }
    for (unsigned i = 0; i < t->n; i++) {
        cur_fn[t->it[i].fn] += t->it[i].cost;
        cur_total += t->it[i].cost;
    }
}

static void close_window(void)
{
    g_array_append_val(labels, open_label);
    g_array_append_val(totals, cur_total);
    if (open_label < from_label) {
        return;
    }
    for (int j = 0; j < MAXFN; j++) {
        sum_fn[j] += cur_fn[j];
    }
    int slot = -1;
    for (int k = 0; k < KEEP; k++) {
        if (!top[k]) {
            top[k] = g_new0(struct win, 1);
            slot = k;
            break;
        }
        if (slot < 0 || top[k]->total < top[slot]->total) {
            slot = k;
        }
    }
    if (top[slot]->total <= cur_total) {
        top[slot]->label = open_label;
        top[slot]->total = cur_total;
        memcpy(top[slot]->fn, cur_fn, sizeof(cur_fn));
    }
}

static void on_mark(unsigned int vcpu, void *udata)
{
    g_autoptr(GByteArray) b = g_byte_array_new();
    int32_t v = 0;

    qemu_plugin_read_register(r0, b);
    memcpy(&v, b->data, 4);
    if (v < 0) {
        if (open_label >= 0) {
            close_window();
        }
        open_label = -1;
        return;
    }
    open_label = v;
    cur_total = 0;
    memset(cur_fn, 0, sizeof(cur_fn));
}

static void on_tb(struct qemu_plugin_tb *tb, void *udata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    struct tbc *t = g_malloc0(sizeof(*t) + n * sizeof(struct item));

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t a = qemu_plugin_insn_vaddr(insn);
        uintptr_t e = (uintptr_t)g_hash_table_lookup(table, GUINT_TO_POINTER((guint)a));
        uint32_t fn = e ? (uint32_t)(e >> 8) : MAXFN - 1, cost = e ? (uint32_t)(e & 0xff) : 1;

        if (t->n && t->it[t->n - 1].fn == fn) {
            t->it[t->n - 1].cost += cost;
        } else {
            t->it[t->n].fn = fn;
            t->it[t->n++].cost = cost;
        }
    }
    if (qemu_plugin_tb_vaddr(tb) == mark_addr) {
        qemu_plugin_register_vcpu_tb_exec_cb(tb, on_mark, QEMU_PLUGIN_CB_R_REGS, NULL);
    }
    qemu_plugin_register_vcpu_tb_exec_cb(tb, on_exec, QEMU_PLUGIN_CB_NO_REGS, t);
}

static void on_init(unsigned int vcpu, void *udata)
{
    g_autoptr(GArray) regs = qemu_plugin_get_registers();

    for (guint i = 0; i < regs->len; i++) {
        qemu_plugin_reg_descriptor *d = &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (!strcmp(d->name, "r0")) {
            r0 = d->handle;
        }
    }
    if (!r0) {
        fprintf(stderr, "blockcost: no r0\n");
        exit(1);
    }
}

static void on_exit(void *udata)
{
    FILE *f = fopen(out_path, "w");

    if (!f) {
        return;
    }
    for (guint i = 0; i < labels->len; i++) {
        fprintf(f, "W %d %" PRIu64 "\n", g_array_index(labels, int32_t, i), g_array_index(totals, uint64_t, i));
    }
    for (int k = 0; k < KEEP; k++) {
        if (!top[k]) {
            continue;
        }
        fprintf(f, "T %d %" PRIu64 "\n", top[k]->label, top[k]->total);
        for (int j = 0; j < MAXFN; j++) {
            if (top[k]->fn[j]) {
                fprintf(f, "F %d %" PRIu64 "\n", j, top[k]->fn[j]);
            }
        }
    }
    for (int j = 0; j < MAXFN; j++) {
        if (sum_fn[j]) {
            fprintf(f, "S %d %" PRIu64 "\n", j, sum_fn[j]);
        }
    }
    fclose(f);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                                           int argc, char **argv)
{
    const char *costs = NULL;

    for (int i = 0; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "costs=")) {
            costs = argv[i] + 6;
        } else if (g_str_has_prefix(argv[i], "mark=")) {
            mark_addr = g_ascii_strtoull(argv[i] + 5, NULL, 16);
        } else if (g_str_has_prefix(argv[i], "from=")) {
            from_label = (int)g_ascii_strtoll(argv[i] + 5, NULL, 10);
        } else if (g_str_has_prefix(argv[i], "out=")) {
            out_path = argv[i] + 4;
        }
    }
    FILE *f = costs ? fopen(costs, "r") : NULL;
    if (!f || !mark_addr || !out_path) {
        fprintf(stderr, "blockcost: need costs=, mark=, out=\n");
        return -1;
    }
    table = g_hash_table_new(g_direct_hash, g_direct_equal);
    unsigned a, c, fn;
    while (fscanf(f, "%x %u %u", &a, &c, &fn) == 3) {
        g_hash_table_insert(table, GUINT_TO_POINTER(a),
                            (gpointer)(uintptr_t)(((uintptr_t)(fn < MAXFN - 1 ? fn : MAXFN - 2) << 8) | (c & 0xff)));
    }
    fclose(f);
    labels = g_array_new(FALSE, FALSE, sizeof(int32_t));
    totals = g_array_new(FALSE, FALSE, sizeof(uint64_t));
    qemu_plugin_register_vcpu_init_cb(id, on_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, on_tb, NULL);
    qemu_plugin_register_atexit_cb(id, on_exit, NULL);
    return 0;
}
