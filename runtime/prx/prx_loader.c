/*
 * prx_loader.c — see prx_loader.h for the design.
 *
 * Two steps per module: place the relocated image into guest RAM at its load
 * base, then register every lifted function in the host dispatch table.
 */
#include "prx_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime's guest memory: host_ptr = vm_base + guest_addr. Declared by
 * runtime/memory/vm.h; we only need the symbol, so extern it directly to keep
 * the loader independent of the memory backend's init path. */
extern uint8_t* vm_base;

/* ---- export registry ---------------------------------------------------- *
 * Flat NID -> guest-address table, filled as modules load. A handful of system
 * PRXs export a few hundred symbols total.
 *
 * The lookup is on every HLE call, not once per import: ps3_hle_call asks it
 * whether a loaded PRX exports the NID before it runs the HLE stub, and the
 * answer for most calls is no (sys_lwmutex_*, sys_cond_*, ...), which a
 * linear scan found only at the end of the whole table. Demon's Souls'
 * HighGraphics makes a few hundred thousand HLE calls a second. A hash index
 * over the table answers in a probe or two. */
typedef struct { uint32_t nid; uint32_t guest_addr; } prx_export_reg_entry;
#define PRX_EXPORT_REG_CAP 2048
static prx_export_reg_entry s_export_reg[PRX_EXPORT_REG_CAP];
static uint32_t             s_export_reg_n = 0;
/* Open addressing, linear probing; a slot holds an entry's index + 1, 0 when
 * empty. Twice the table's capacity, so a probe run stays short. Entries are
 * only added (an existing NID is overwritten in place), and a slot is
 * published after its entry is written, so a lookup racing a module load
 * sees either no entry or a complete one. */
#define PRX_EXPORT_HASH_BITS 12
static uint32_t s_export_hash[1u << PRX_EXPORT_HASH_BITS];

static uint32_t prx_export_slot(uint32_t nid)
{
    return (nid * 0x9E3779B1u) >> (32 - PRX_EXPORT_HASH_BITS);
}

/* The entry index for nid, or -1. */
static int prx_export_find(uint32_t nid)
{
    const uint32_t mask = (1u << PRX_EXPORT_HASH_BITS) - 1;
    for (uint32_t h = prx_export_slot(nid);; h = (h + 1) & mask) {
        const uint32_t v = __atomic_load_n(&s_export_hash[h], __ATOMIC_ACQUIRE);
        if (!v) return -1;
        if (s_export_reg[v - 1].nid == nid) return (int)(v - 1);
    }
}

static void prx_export_register(uint32_t nid, uint32_t guest_addr)
{
    /* Last writer wins if two modules export the same NID (matches the PS3
     * loader's link order). Overwrite an existing entry rather than duplicate. */
    const int i = prx_export_find(nid);
    if (i >= 0) {
        __atomic_store_n(&s_export_reg[i].guest_addr, guest_addr, __ATOMIC_RELEASE);
        return;
    }
    if (s_export_reg_n >= PRX_EXPORT_REG_CAP) {
        fprintf(stderr, "[prx] export registry full, dropping NID 0x%08X\n", nid);
        return;
    }
    const uint32_t n = s_export_reg_n++;
    s_export_reg[n] = (prx_export_reg_entry){ nid, guest_addr };
    const uint32_t mask = (1u << PRX_EXPORT_HASH_BITS) - 1;
    uint32_t h = prx_export_slot(nid);
    while (s_export_hash[h]) h = (h + 1) & mask;
    __atomic_store_n(&s_export_hash[h], n + 1, __ATOMIC_RELEASE);
}

uint32_t prx_resolve_export(uint32_t nid)
{
    const int i = prx_export_find(nid);
    return i < 0 ? 0 : __atomic_load_n(&s_export_reg[i].guest_addr, __ATOMIC_ACQUIRE);
}

uint32_t prx_export_registry_count(void) { return s_export_reg_n; }

prx_load_result prx_load_module(const prx_module* m, prx_register_fn reg)
{
    prx_load_result r;
    memset(&r, 0, sizeof(r));

    if (!m || !m->image || m->image_size == 0) {
        fprintf(stderr, "[prx] load failed: null/empty module\n");
        return r;
    }
    if (!vm_base) {
        fprintf(stderr, "[prx] load failed: vm_base not initialized "
                        "(call vm_init before loading PRX modules)\n");
        return r;
    }

    r.base       = m->base;
    r.image_size = m->image_size;

    /* Step A — place the relocated image into guest RAM at the load base.
     * The bytes already carry base-applied addresses (prx_relocate.py), so the
     * module's data, OPDs and pointer tables are immediately consistent with
     * the lifted code and the OPD-walking dispatcher. */
    memcpy(vm_base + m->base, m->image, m->image_size);

    /* Step B — register every lifted function at its guest address. The lift
     * baked the load base in (--base B), so e->addr is already B + offset and
     * needs no adjustment here. A null host function (rare: a table hole) is
     * skipped rather than registered. */
    if (reg) {
        for (uint64_t i = 0; i < m->func_count; i++) {
            const prx_func_entry* e = &m->funcs[i];
            if (!e->func)
                continue;
            reg((uint32_t)e->addr, e->func);
            r.funcs_registered++;
        }
    }

    /* Step C — publish exports as NID -> (base + vaddr) so a title's import
     * resolver can dispatch into this module by NID. Function exports point at
     * an OPD in the freshly-placed image; ps3_indirect_call walks it. */
    uint32_t exports_published = 0;
    if (m->exports) {
        for (uint32_t i = 0; i < m->export_count; i++) {
            const prx_export* e = &m->exports[i];
            if (e->nid == 0)
                continue;
            prx_export_register(e->nid, m->base + e->vaddr);
            exports_published++;
        }
    }

    r.ok = 1;
    fprintf(stderr,
            "[prx] loaded %-12s base=0x%08X size=%u (0x%X) funcs=%u/%llu exports=%u\n",
            m->name ? m->name : "?", m->base, m->image_size, m->image_size,
            r.funcs_registered, (unsigned long long)m->func_count,
            exports_published);
    return r;
}

uint8_t* prx_image_load_file(const char* path, uint32_t* out_size)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[prx] cannot open image '%s'\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "[prx] short read on '%s'\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if (out_size)
        *out_size = (uint32_t)n;
    return buf;
}
