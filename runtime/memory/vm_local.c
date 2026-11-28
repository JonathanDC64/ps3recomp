/*
 * ps3recomp - RSX local (video) memory's place in the guest space (vm.h: vm_rsx_local_size, vm_rsx_local_base)
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS   /* getenv */
#endif
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>

#define VM_LOCAL_MB_DEFAULT 256u
#define VM_LOCAL_MB_MAX     1024u   /* cellGcmSys.c's s_local_offset_page: 1,024 pages of 1 MB */

static volatile uint32_t s_local_size;   /* 0 until the first call */

uint32_t vm_rsx_local_size(void)
{
    uint32_t size = s_local_size;
    if (size) return size;
    uint32_t mb = VM_LOCAL_MB_DEFAULT;
    const char* e = getenv("PS3_LOCAL_MB");
    if (e && *e) {
        char* end = NULL;
        const unsigned long v = strtoul(e, &end, 10);
        if (end != e && !*end && v >= VM_LOCAL_MB_DEFAULT && v <= VM_LOCAL_MB_MAX)
            mb = (uint32_t)v;
        else
            fprintf(stderr, "[vm] PS3_LOCAL_MB=%s is not a size from %u to %u (MB): %u\n", e,
                    VM_LOCAL_MB_DEFAULT, VM_LOCAL_MB_MAX, VM_LOCAL_MB_DEFAULT);
    }
    size = mb << 20;
    if (mb != VM_LOCAL_MB_DEFAULT)
        fprintf(stderr, "[vm] RSX local memory: %u MB at 0x%08X (PS3_LOCAL_MB)\n", mb, VM_STACK_BASE - size);
    /* (The first call is the main thread's, before any other thread: a later one only reads.) */
    s_local_size = size;
    return size;
}

uint32_t vm_rsx_local_base(void)
{
    return VM_STACK_BASE - vm_rsx_local_size();
}

uint32_t vm_rsx_local_wb_size(void)
{
    const uint32_t size = vm_rsx_local_size();
    return size < VM_RSX_LOCAL_WB_MAX ? size : VM_RSX_LOCAL_WB_MAX;
}
