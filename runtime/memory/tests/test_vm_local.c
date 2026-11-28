/*
 * ps3recomp - RSX local memory's size and base (runtime/memory/vm_local.c)
 *
 * test_vm_local <expected MB> [PS3_LOCAL_MB's value]: the knob is read once, so each value is its own run (CMake's
 * add_test lines). The size is the knob's, or 256 MB for none or a bad one; it always ends at the stacks.
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
        snprintf(env, sizeof env, "PS3_LOCAL_MB=%s", argv[2]);
#ifdef _WIN32
        _putenv(env);
#else
        putenv(env);
#endif
    }
    const uint32_t size = vm_rsx_local_size();
    assert(size == (uint32_t)want << 20);
    assert(vm_rsx_local_base() + size == VM_STACK_BASE);
    assert(vm_rsx_local_size() == size);   /* (read once) */
    if (want == 256) assert(vm_rsx_local_base() == 0xC0000000u);   /* a PS3's */
    printf("test_vm_local: %s -> %u MB at 0x%08X ok\n", argc == 3 ? argv[2] : "(none)", want, vm_rsx_local_base());
    return 0;
}
