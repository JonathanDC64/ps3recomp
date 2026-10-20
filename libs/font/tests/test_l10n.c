/*
 * test_l10n - cellL10n's guest-facing UTF8toUTF16: one character per call,
 * big-endian UTF-16 in guest memory, the byte count as the return value.
 *
 * Self-contained: it compiles cellL10n.c into this file and gives it a small
 * guest arena.
 *
 *   clang-cl /I include /Fe:test_l10n.exe libs/font/tests/test_l10n.c
 *   cc -std=gnu17 -I include -o test_l10n libs/font/tests/test_l10n.c
 */

#include "../cellL10n.c"

#include <assert.h>
#include <stdlib.h>

/* ppu_memory.h hooks the runtime supplies; inert here. */
uint8_t* vm_base;
int g_resv_store_active = 0;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int  spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_lockline_lock_ea(uint32_t ea, uint32_t len) { (void)ea; (void)len; }
void spu_lockline_unlock_ea(uint32_t ea, uint32_t len) { (void)ea; (void)len; }
void spu_coh_notify_write(uint32_t addr) { (void)addr; }
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width) { (void)addr; (void)val; (void)width; }

enum { SRC = 0x1000, DST = 0x2000, LEN = 0x3000 };

/* Converts the bytes at SRC once; returns what UTF8toUTF16 returned. */
static s32 one(const char* bytes, u32 n)
{
    memset(vm_base + SRC, 0, 16);
    memset(vm_base + DST, 0xEE, 8);
    memset(vm_base + LEN, 0xEE, 4);
    memcpy(vm_base + SRC, bytes, n);
    return UTF8toUTF16(SRC, DST, LEN);
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 1u << 16);
    assert(vm_base);

    /* ASCII, 2-, 3- and 4-byte sequences: bytes consumed, units stored
     * big-endian, the unit count at dst_len. */
    assert(one("A", 1) == 1);
    assert(vm_read16(DST) == 0x0041 && vm_read32(LEN) == 1);
    assert(vm_base[DST] == 0x00 && vm_base[DST + 1] == 0x41);
    assert(one("\xC3\xA9", 2) == 2 && vm_read16(DST) == 0x00E9 && vm_read32(LEN) == 1);
    assert(one("\xE2\x82\xAC", 3) == 3 && vm_read16(DST) == 0x20AC && vm_read32(LEN) == 1);
    assert(one("\xF0\x9F\x98\x80", 4) == 4);
    assert(vm_read16(DST) == 0xD83D && vm_read16(DST + 2) == 0xDE00 && vm_read32(LEN) == 2);

    /* Illegal input answers 0 and leaves dst alone: a stray continuation
     * byte, a lead byte whose sequence stops short, an overlong form, a
     * UTF-16 surrogate encoded in UTF-8. */
    assert(one("\x80", 1) == 0 && vm_read32(LEN) == 0xEEEEEEEE);
    assert(one("\xC3" "A", 2) == 0);
    assert(one("\xE2\x82", 2) == 0);   /* the terminator ends it */
    assert(one("\xC0\x80", 2) == 0);
    assert(one("\xED\xA0\x80", 3) == 0);
    assert(vm_read16(DST) == 0xEEEE);

    /* A title's loop over a string, as Demon's Souls does with the server's
     * login messages: advance by the return value until the terminator. */
    {
        const char* msg = "Caf\xC3\xA9 \xE2\x82\xAC 5\r\n";
        const u16 want[] = { 'C', 'a', 'f', 0xE9, ' ', 0x20AC, ' ', '5', '\r', '\n' };
        u32 n = (u32)strlen(msg), pos = 0, out = 0;
        memcpy(vm_base + SRC, msg, n + 1);
        while (vm_read8(SRC + pos)) {
            s32 used = UTF8toUTF16(SRC + pos, DST + 2 * out, LEN);
            assert(used > 0);
            pos += (u32)used;
            out += vm_read32(LEN);
        }
        assert(pos == n && out == sizeof want / sizeof want[0]);
        for (u32 i = 0; i < out; i++) assert(vm_read16(DST + 2 * i) == want[i]);
    }

    /* Null addresses: nothing converted. */
    assert(UTF8toUTF16(0, DST, LEN) == 0);
    assert(UTF8toUTF16(SRC, 0, LEN) == 0);
    assert(UTF8toUTF16(SRC, DST, 0) == 0);

    /* The library's own string path still converts through the host helper. */
    {
        u16 buf[8]; u32 len = 8;
        assert(l10n_utf8_to_utf16((const u8*)"Hi\xC3\xA9", 4, buf, &len) == CELL_OK);
        assert(len == 3 && buf[0] == 'H' && buf[2] == 0xE9);
    }

    printf("test_l10n: all checks passed\n");
    return 0;
}
