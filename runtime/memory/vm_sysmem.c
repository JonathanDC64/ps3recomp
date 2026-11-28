/*
 * ps3recomp - the sys_memory window's place in the guest space (vm.h: vm_sysmem_size, vm_sysmem_base)
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS   /* getenv */
#endif
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>

#define VM_SYSMEM_MB_DEFAULT 256u
#define VM_SYSMEM_MB_MAX     511u   /* from 0x30100000: right above a PRX lifted at 0x30000000 */

static volatile uint32_t s_sysmem_size;   /* 0 until the first call */

uint32_t vm_sysmem_size(void)
{
    uint32_t size = s_sysmem_size;
    if (size) return size;
    uint32_t mb = VM_SYSMEM_MB_DEFAULT;
    const char* e = getenv("PS3_SYSMEM_MB");
    if (e && *e) {
        char* end = NULL;
        const unsigned long v = strtoul(e, &end, 10);
        if (end != e && !*end && v >= VM_SYSMEM_MB_DEFAULT && v <= VM_SYSMEM_MB_MAX)
            mb = (uint32_t)v;
        else
            fprintf(stderr, "[vm] PS3_SYSMEM_MB=%s is not a size from %u to %u (MB): %u\n", e,
                    VM_SYSMEM_MB_DEFAULT, VM_SYSMEM_MB_MAX, VM_SYSMEM_MB_DEFAULT);
    }
    size = mb << 20;
    if (mb != VM_SYSMEM_MB_DEFAULT)
        fprintf(stderr, "[vm] sys_memory window: %u MB at 0x%08X (PS3_SYSMEM_MB)\n", mb, VM_SYSMEM_END - size);
    /* (The first call is the main thread's, before any other thread: a later one only reads.) */
    s_sysmem_size = size;
    return size;
}

uint32_t vm_sysmem_base(void)
{
    return VM_SYSMEM_END - vm_sysmem_size();
}
