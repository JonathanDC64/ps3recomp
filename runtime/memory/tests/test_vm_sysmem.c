/*
 * ps3recomp - the sys_memory window's size and base (runtime/memory/vm_sysmem.c)
 *
 * test_vm_sysmem <expected MB> [PS3_SYSMEM_MB's value]: the knob is read once, so each value is its own run (CMake's
 * add_test lines). The size is the knob's, or 256 MB for none or a bad one; it always ends at 0x50000000.
 */
#define _CRT_SECURE_NO_WARNINGS   /* putenv */
#include "vm.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv)
{
    assert(argc == 2 || argc == 3);
    const unsigned want = (unsigned)strtoul(argv[1], NULL, 10);
    static char env[64];
    if (argc == 3) {
        snprintf(env, sizeof env, "PS3_SYSMEM_MB=%s", argv[2]);
#ifdef _WIN32
        _putenv(env);
#else
        putenv(env);
#endif
    }
    const uint32_t size = vm_sysmem_size();
    assert(size == (uint32_t)want << 20);
    assert(vm_sysmem_base() + size == VM_SYSMEM_END);
    assert(vm_sysmem_size() == size);   /* (read once) */
    if (want == 256) assert(vm_sysmem_base() == 0x40000000u);   /* a PS3's */
    if (want == 511) assert(vm_sysmem_base() == 0x30100000u);   /* the most: right above 0x30000000's PRX */
    printf("test_vm_sysmem: %s -> %u MB at 0x%08X ok\n", argc == 3 ? argv[2] : "(none)", want, vm_sysmem_base());
    return 0;
}
