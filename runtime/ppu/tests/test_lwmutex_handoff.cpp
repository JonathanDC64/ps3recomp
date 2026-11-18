/* sys_lwmutex with PPU_LWM_HANDOFF (ppu_sysprx.cpp): a thread that unlocks
 * and locks again at once must not keep a sleeper out.
 *
 * Two host threads stand in for Demon's Souls' MultiStream (#403, #502): a
 * "mixer" that holds the mutex for a round, unlocks and locks again at once,
 * as MOMixerThread does when it is behind, and a "main" thread that locks it
 * now and then, as MOMainThread does. For each of main's locks the test counts
 * the mixer rounds that began while main waited.
 *
 * - Both modes: the mutex excludes (a counter both threads bump under it).
 * - PPU_LWM_HANDOFF=1: main gets the mutex at the end of the round it waited
 *   behind. Asserted on the share of its locks that waited at most two rounds
 *   (a count of rounds, not a time, so a slow or busy machine passes too).
 * - PPU_LWM_HANDOFF=0 (as before) and a SYS_SYNC_RETRY mutex with the
 *   hand-off on: printed only. There main usually waits until the mixer stops.
 *
 * It includes ppu_sysprx.cpp, with stand-ins for what that file calls
 * elsewhere in the runtime. Windows (clang-cl) and Linux. */
#include "../ppu_sysprx.cpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

/* ---- stand-ins for the rest of the runtime ---- */
static uint8_t s_mem[0x10000];
extern "C" uint8_t* vm_base = s_mem;
extern "C" uint32_t vm_read32(uint64_t a)
{
    const uint8_t* p = s_mem + (a & 0xFFFCu);
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
extern "C" uint64_t vm_read64(uint64_t a) { return (uint64_t)vm_read32(a) << 32 | vm_read32(a + 4); }
extern "C" void vm_write32(uint64_t a, uint32_t v)
{
    uint8_t* p = s_mem + (a & 0xFFFCu);
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
extern "C" void vm_write64(uint64_t a, uint64_t v) { vm_write32(a, (uint32_t)(v >> 32)); vm_write32(a + 4, (uint32_t)v); }
extern "C" void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) {}
extern "C" uint64_t lv2_system_time_usec(void) { return 0; }
extern "C" volatile long long g_gcm_ref_pub_count = 0;
extern "C" unsigned long long ppu_thread_cpu_us(unsigned) { return 0; }
extern "C" unsigned ppu_thread_prof_pc(unsigned) { return 0; }
extern "C" int64_t sys_ppu_thread_create(ppu_context*) { return 0; }
extern "C" int64_t sys_ppu_thread_exit(ppu_context*) { return 0; }
extern "C" unsigned int cellGcmSetupContext(unsigned int, unsigned int, unsigned int, unsigned int,
                                           CellGcmGuestAlloc, CellGcmGuestWrite32) { return 0; }
extern "C" void cellGcm_fifo_recycle(unsigned int) {}
extern "C" void ppu_register_function(uint64_t, void (*)(ppu_context*)) {}
extern "C" void ps3_net_host_register(unsigned int (*)(unsigned int, unsigned int)) {}
extern "C" void np_score_register_ctx(void) {}
extern "C" void cell_audio_register_ctx(void) {}
extern "C" void spu_raw_note_image(uint32_t, uint32_t) {}
extern "C" int ps3_net_online(void) { return 0; }
#ifndef _WIN32
/* win32_compat.c's, the only one of its out-of-line functions the lwmutex uses */
extern "C" void SetLastError(DWORD) {}
extern "C" DWORD GetLastError(void) { return 0; }
#endif

/* ---- the test ---- */
static const uint32_t kLwm = 0x1000, kAttr = 0x1100;
static int s_failed;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_failed = 1; } } while (0)

static void set_handoff(const char* v)
{
#ifdef _WIN32
    _putenv_s("PPU_LWM_HANDOFF", v);
#else
    setenv("PPU_LWM_HANDOFF", v, 1);
#endif
    ps3_env_gen = ps3_env_gen + 1;   /* the knob reads the variable again (PS3_KNOB) */
}

static void hold_us(unsigned us)   /* busy, as a thread doing its work under the mutex */
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < end) {}
}

static void lock(ppu_context* c)
{
    c->gpr[3] = kLwm; c->gpr[4] = 0;   /* no timeout */
    sys_lwmutex_lock(c);
    CHECK(c->gpr[3] == 0);
}

static void unlock(ppu_context* c)
{
    c->gpr[3] = kLwm;
    sys_lwmutex_unlock(c);
}

struct Result { unsigned locks, within2, worst, rounds; };

/* protocol: the mutex's (2 SYS_SYNC_PRIORITY, 4 SYS_SYNC_RETRY). max_rounds:
 * the mixer stops after this many, so a starved main thread still finishes. */
static Result run(uint32_t protocol, unsigned main_locks, unsigned max_rounds)
{
    memset(s_mem, 0, sizeof s_mem);
    vm_write32(kAttr, protocol);
    static ppu_context mk, mm;   /* (large: not on the stack) */
    memset(&mk, 0, sizeof mk); memset(&mm, 0, sizeof mm);
    mk.thread_id = 13; mm.thread_id = 12;
    mk.gpr[3] = kLwm; mk.gpr[4] = kAttr;
    sys_lwmutex_create(&mk);
    CHECK(mk.gpr[3] == 0);

    std::atomic<unsigned> rounds{0};
    std::atomic<int> stop{0};
    unsigned shared = 0, mixer_adds = 0, main_adds = 0;   /* bumped only under the mutex */
    std::thread mixer([&] {
        while (!stop.load() && rounds.load() < max_rounds) {
            lock(&mk);
            rounds.fetch_add(1);
            shared++; mixer_adds++;
            hold_us(300);   /* the round: waiting for the synth's replies */
            unlock(&mk);    /* and the next round at once */
        }
    });
    Result r = { 0, 0, 0, 0 };
    for (unsigned i = 0; i < main_locks; i++) {
        const unsigned before = rounds.load();
        lock(&mm);
        const unsigned waited = rounds.load() - before;
        shared++; main_adds++;
        hold_us(20);
        unlock(&mm);
        r.locks++;
        if (waited <= 2) r.within2++;
        if (waited > r.worst) r.worst = waited;
        hold_us(200);   /* main's own work, outside the mutex */
    }
    stop.store(1);
    mixer.join();
    r.rounds = rounds.load();
    CHECK(shared == mixer_adds + main_adds);   /* the mutex excluded */
    return r;
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const unsigned kLocks = 200;

    set_handoff("1");
    const Result on = run(2, kLocks, 100000);
    std::printf("hand-off on:  main %u locks, %u within two mixer rounds, worst %u rounds; mixer %u rounds\n",
                on.locks, on.within2, on.worst, on.rounds);
    CHECK(on.locks == kLocks);
    CHECK(on.within2 * 10 >= on.locks * 9);   /* 90%: a thread can be preempted at the wrong moment */

    const Result retry = run(4, 20, 3000);
    std::printf("SYS_SYNC_RETRY, hand-off on (not handed over): main %u locks, worst %u rounds; mixer %u rounds\n",
                retry.locks, retry.worst, retry.rounds);
    CHECK(retry.locks == 20);

    set_handoff("0");
    const Result off = run(2, 20, 3000);
    std::printf("hand-off off: main %u locks, %u within two mixer rounds, worst %u rounds; mixer %u rounds\n",
                off.locks, off.within2, off.worst, off.rounds);
    CHECK(off.locks == 20);

    std::printf(s_failed ? "test_lwmutex_handoff: FAILED\n" : "test_lwmutex_handoff: ok\n");
    return s_failed;
}
