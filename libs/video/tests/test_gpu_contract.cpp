/*
 * ps3recomp - the GPU layer's contract test, C++ half and main (CMake target
 * test_gpu_contract, built with PS3RECOMP_RSX_HAL=ON; ctest -R gpu_contract)
 *
 * The port's C++ (the menu renderer, the host) includes gpu.h as well, so
 * this half builds the three headers as C++, runs GPU_CMD_CHECK there, sends
 * commands and device calls through the wrappers, and compares every type's
 * size with the C half's. Then it runs the C half.
 */
#include "gpu.h"
#include "gpu_vt_cmd.h"
#include "test_gpu_contract.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;

#define CHECK(cond)                                                                   \
    do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
                        g_fail++; } } while (0)

/* A few of the numbers, as C++ sees them (the C half checks them all). */
static_assert(GPU_FORMAT_R8G8B8A8_UNORM == 28 && GPU_FORMAT_D24_UNORM_S8_UINT == 45, "formats");
static_assert(GPU_STATE_PIXEL_SHADER_RESOURCE == 0x80 && GPU_STATE_GENERIC_READ == 0xAC3, "states");
static_assert(GPU_SWIZZLE_IDENTITY == GPU_SWIZZLE(0, 1, 2, 3), "swizzle");
static_assert(sizeof(gpu_pipeline_desc) == 536, "the pipeline cache's names");

GPU_CMD_CHECK(vt_cpp);

struct gpu_cmd {
    gpu_cmd_head head;
    int          last;
};
#define FAKE_CMD_V(name, ret, params) static void fake_##name params { c->last = GPU_CMD_OP_##name; }
#define FAKE_CMD_R(name, ret, params) static ret fake_##name params { c->last = GPU_CMD_OP_##name; return ret(); }
#define FAKE_CMD(name, form, ret, params, args) FAKE_CMD_##form(name, ret, params)
GPU_CMD_OPS(FAKE_CMD)
#define FAKE_CMD_INIT(name, form, ret, params, args) fake_##name,
static const gpu_cmd_ops k_fake_ops = { GPU_CMD_OPS(FAKE_CMD_INIT) };

static const char* const k_type_names[] = {
#define TYPE_NAME(t) #t,
    GPU_CONTRACT_TYPES(TYPE_NAME)
#undef TYPE_NAME
};
static const size_t k_cpp_sizes[] = {
#define CPP_SIZE(t) sizeof(t),
    GPU_CONTRACT_TYPES(CPP_SIZE)
#undef CPP_SIZE
};
static const int k_types = int(sizeof k_cpp_sizes / sizeof k_cpp_sizes[0]);

int main()
{
    /* The same structures in both languages. */
    size_t c_sizes[sizeof k_cpp_sizes / sizeof k_cpp_sizes[0]];
    CHECK(gpu_contract_c_sizes(c_sizes, k_types) == k_types);
    for (int i = 0; i < k_types; i++)
        if (c_sizes[i] != k_cpp_sizes[i]) {
            std::printf("FAIL %s: %zu bytes in C, %zu in C++\n", k_type_names[i], c_sizes[i], k_cpp_sizes[i]);
            g_fail++;
        }

    /* Commands from C++. */
    gpu_cmd rec;
    std::memset(&rec, 0, sizeof rec);
    rec.head.ops = &k_fake_ops;
    gpu_cmd* const c = &rec;
    gpu_viewport vp{};
    gpu_pipeline pso{};
    const float factor[4] = {1, 1, 1, 1};
    gpu_cmd_set_viewport(c, &vp);          CHECK(rec.last == GPU_CMD_OP_set_viewport);
    gpu_cmd_set_pipeline(c, pso);          CHECK(rec.last == GPU_CMD_OP_set_pipeline);
    gpu_cmd_set_blend_factor(c, factor);   CHECK(rec.last == GPU_CMD_OP_set_blend_factor);
    gpu_cmd_set_stencil_ref(c, 1);         CHECK(rec.last == GPU_CMD_OP_set_stencil_ref);
    gpu_cmd_draw_indexed(c, 6, 1, 0, 0, 0); CHECK(rec.last == GPU_CMD_OP_draw_indexed);
    gpu_cmd_resolve(c, gpu_texture{1}, 0, gpu_texture{2}, 0, GPU_FORMAT_R8G8B8A8_UNORM);
                                           CHECK(rec.last == GPU_CMD_OP_resolve);

    const int c_fail = gpu_contract_c_run();
    if (g_fail || c_fail) {
        std::printf("GPU contract: %d FAILED\n", g_fail + c_fail);
        return 1;
    }
    std::printf("GPU contract: OK (%d commands, %d types)\n", int(GPU_CMD_OP_COUNT), k_types);
    return 0;
}
