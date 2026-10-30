/*
 * ps3recomp - the portable renderer's GPU layer: the commands a recorder takes
 *
 * One list, as X(name, form, ret, params, args): form V returns nothing, R
 * returns ret; the first parameter is always the recorder, named c. Every
 * command only records, so all of them are V today.
 *
 * Each command is gpu_cmd_<name>(c, ...), and there are two kinds of file
 * that define them:
 *   - gpu_vt_cmd.h: through the recorder's table, for code compiled once for
 *     every backend (blits, AA, HDR, write-back, captures: a few calls a
 *     frame);
 *   - a backend's gpu_<backend>_cmd.h: static inline, straight onto the API,
 *     for the draw loop (rsxc_frame.inc is compiled once per backend with its
 *     header), so a draw makes direct calls and no table hops.
 * A file includes one of them, never both. GPU_CMD_CHECK() then proves that
 * every command is there with the right parameters, and GPU_CMD_OPS_TABLE
 * fills a backend's table from its static inline versions.
 *
 * The draw loop also makes a few device calls per draw: it writes its
 * textures and samplers into tables and names the tables and buffers it
 * binds. Those are GPU_FAST_FUNCS below, each gpu_fast_<name>(d, ...) with
 * the parameters of the gpu.h function of the same name, defined by the same
 * two files: through the device's table in gpu_vt_cmd.h, and static inline in
 * a backend's header (on D3D12 the memo's compare and the address arithmetic
 * inline, the descriptor write out of line). GPU_CMD_CHECK checks them too.
 *
 * The numbers they take are D3D12's (gpu.h), and so is what they do; where
 * Vulkan differs, its backend makes up the difference:
 *   - a pass is the targets bound (OMSetRenderTargets). pass_begin may follow
 *     pass_begin with no pass_end between; on Vulkan any barrier, copy or
 *     query reset or resolve ends the pass, and the next draw or clear opens
 *     it again, keeping the targets' contents;
 *   - set_layout forgets every slot's binding, as SetGraphicsRootSignature
 *     does;
 *   - clears clear the whole view, or one rectangle of it, and may name a
 *     view that is not bound (on Vulkan it must be bound or the clear ends
 *     the pass).
 */
#ifndef PS3RECOMP_GPU_CMD_OPS_H
#define PS3RECOMP_GPU_CMD_OPS_H

#include "gpu.h"

#ifdef __cplusplus
#include <type_traits>   /* GPU_CMD_CHECK */
extern "C" {
#endif

#define GPU_CMD_OPS(X) \
    /* Targets: up to four colour views and a depth view (OMSetRenderTargets). */ \
    X(pass_begin,      V, void, (gpu_cmd* c, const gpu_pass* pass), (c, pass)) \
    /* Optional: says the pass is over (Vulkan ends rendering; D3D12 has nothing to do). */ \
    X(pass_end,        V, void, (gpu_cmd* c), (c)) \
    /* rect NULL: all of it. */ \
    X(clear_color,     V, void, (gpu_cmd* c, gpu_view rtv, const float rgba[4], const gpu_rect* rect), (c, rtv, rgba, rect)) \
    /* flags GPU_CLEAR_DEPTH and/or GPU_CLEAR_STENCIL. */ \
    X(clear_depth,     V, void, (gpu_cmd* c, gpu_view dsv, uint32_t flags, float depth, uint8_t stencil, const gpu_rect* rect), (c, dsv, flags, depth, stencil, rect)) \
    X(set_layout,      V, void, (gpu_cmd* c, gpu_layout layout), (c, layout)) \
    X(set_pipeline,    V, void, (gpu_cmd* c, gpu_pipeline pipeline), (c, pipeline)) \
    /* GPU_TOPO_*: list or strip within the pipeline's topology type. */ \
    X(set_topology,    V, void, (gpu_cmd* c, uint32_t topology), (c, topology)) \
    /* Depth writes on or off within the pipeline's (Vulkan's dynamic state, for a depth \
     * target bound read-only; D3D12's read-only view does it, so nothing there). */ \
    X(set_depth_write, V, void, (gpu_cmd* c, uint32_t on), (c, on)) \
    X(set_viewport,    V, void, (gpu_cmd* c, const gpu_viewport* vp), (c, vp)) \
    X(set_scissor,     V, void, (gpu_cmd* c, const gpu_rect* rect), (c, rect)) \
    /* The pools tables are bound from (SetDescriptorHeaps); either may be none. */ \
    X(set_table_pools, V, void, (gpu_cmd* c, gpu_table_pool tex, gpu_table_pool samplers), (c, tex, samplers)) \
    /* A CB slot's constants at addr (cb_align aligned); a RAW slot's buffer at addr. */ \
    X(bind_cb,         V, void, (gpu_cmd* c, uint32_t slot, gpu_addr addr), (c, slot, addr)) \
    X(bind_raw,        V, void, (gpu_cmd* c, uint32_t slot, gpu_addr addr), (c, slot, addr)) \
    X(bind_table,      V, void, (gpu_cmd* c, uint32_t slot, gpu_table table), (c, slot, table)) \
    /* count 32-bit values into a CONST32 slot from value first on. */ \
    X(push_const,      V, void, (gpu_cmd* c, uint32_t slot, uint32_t count, const void* values, uint32_t first), (c, slot, count, values, first)) \
    /* One of them (SetGraphicsRoot32BitConstant): the fetch draws' b2, per draw. */ \
    X(push_const1,     V, void, (gpu_cmd* c, uint32_t slot, uint32_t value, uint32_t first), (c, slot, value, first)) \
    /* Vertex buffer slot: size bytes at addr, stride bytes a vertex. */ \
    X(set_vb,          V, void, (gpu_cmd* c, uint32_t slot, gpu_addr addr, uint32_t size, uint32_t stride), (c, slot, addr, size, stride)) \
    /* format GPU_FORMAT_R32_UINT or _R16_UINT. */ \
    X(set_ib,          V, void, (gpu_cmd* c, gpu_addr addr, uint32_t size, gpu_format format), (c, addr, size, format)) \
    X(draw,            V, void, (gpu_cmd* c, uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance), (c, vertices, instances, first_vertex, first_instance)) \
    X(draw_indexed,    V, void, (gpu_cmd* c, uint32_t indices, uint32_t instances, uint32_t first_index, int32_t base_vertex, uint32_t first_instance), (c, indices, instances, first_index, base_vertex, first_instance)) \
    /* One texture's whole move between states, the common case. */ \
    X(barrier,         V, void, (gpu_cmd* c, gpu_texture texture, gpu_state before, gpu_state after), (c, texture, before, after)) \
    X(barrier_n,       V, void, (gpu_cmd* c, uint32_t n, const gpu_barrier* list), (c, n, list)) \
    /* Texture to texture (CopyTextureRegion): src's box (NULL: all of it) to x, y, z. */ \
    X(copy_texture,    V, void, (gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z, gpu_texture src, uint32_t src_sub, const gpu_box* box), (c, dst, dst_sub, x, y, z, src, src_sub, box)) \
    /* Every subresource, same size and format (CopyResource). */ \
    X(copy_texture_whole, V, void, (gpu_cmd* c, gpu_texture dst, gpu_texture src), (c, dst, src)) \
    /* Buffer to texture: the bytes `from` describes, to x, y, z. */ \
    X(copy_buf_to_tex, V, void, (gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z, gpu_buffer src, const gpu_footprint* from), (c, dst, dst_sub, x, y, z, src, from)) \
    /* Texture to buffer: src's box laid out as `to` says. NULL: the whole subresource (D3D12's \
     * NULL box), which must be the footprint's size. */ \
    X(copy_tex_to_buf, V, void, (gpu_cmd* c, gpu_buffer dst, const gpu_footprint* to, gpu_texture src, uint32_t src_sub, const gpu_box* box), (c, dst, to, src, src_sub, box)) \
    X(copy_buffer,     V, void, (gpu_cmd* c, gpu_buffer dst, uint64_t dst_offset, gpu_buffer src, uint64_t src_offset, uint64_t size), (c, dst, dst_offset, src, src_offset, size)) \
    /* An MSAA texture's samples averaged into a single-sample one (ResolveSubresource). */ \
    X(resolve,         V, void, (gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, gpu_texture src, uint32_t src_sub, gpu_format format), (c, dst, dst_sub, src, src_sub, format)) \
    /* Before a query's first use in a recording (Vulkan; nothing on D3D12). */ \
    X(query_reset,     V, void, (gpu_cmd* c, gpu_query_pool pool, uint32_t first, uint32_t count), (c, pool, first, count)) \
    X(query_begin,     V, void, (gpu_cmd* c, gpu_query_pool pool, uint32_t index), (c, pool, index)) \
    X(query_end,       V, void, (gpu_cmd* c, gpu_query_pool pool, uint32_t index), (c, pool, index)) \
    /* count results as uint64_t into dst at offset (a readback buffer, in COPY_DEST). */ \
    X(query_resolve,   V, void, (gpu_cmd* c, gpu_query_pool pool, uint32_t first, uint32_t count, gpu_buffer dst, uint64_t offset), (c, pool, first, count, dst, offset)) \
    X(set_stencil_ref, V, void, (gpu_cmd* c, uint32_t ref), (c, ref)) \
    /* For GPU_BLEND_BLEND_FACTOR and _INV_BLEND_FACTOR. */ \
    X(set_blend_factor, V, void, (gpu_cmd* c, const float rgba[4]), (c, rgba)) \
    /* len bytes of text where the API's crash tools read it (DRED's breadcrumbs, a Vulkan \
     * checkpoint), for the draw that follows. */ \
    X(marker,          V, void, (gpu_cmd* c, const char* text, uint32_t len), (c, text, len))

/* The per-draw device calls: gpu_fast_<name>, the gpu_vt function <name>
 * with the same parameters. Vulkan has the same four: its memo in front of
 * vkUpdateDescriptorSets, a set from the pool's region, a device address. */
#define GPU_FAST_FUNCS(X) \
    X(table_write_tex,     V, void,      (gpu_device* d, gpu_table_pool pool, uint32_t index, const gpu_view_desc* srv), (d, pool, index, srv)) \
    X(table_write_sampler, V, void,      (gpu_device* d, gpu_table_pool pool, uint32_t index, gpu_sampler s), (d, pool, index, s)) \
    X(table_ref,           R, gpu_table, (gpu_device* d, gpu_table_pool pool, uint32_t index), (d, pool, index)) \
    X(buffer_addr,         R, gpu_addr,  (gpu_device* d, gpu_buffer b), (d, b))

/* The recorder's command table, in the list's order. */
#define GPU__CMD_MEMBER(name, form, ret, params, args) ret (*name) params;
typedef struct gpu_cmd_ops {
    GPU_CMD_OPS(GPU__CMD_MEMBER)
} gpu_cmd_ops;
#undef GPU__CMD_MEMBER

/* Each command's place in the list: GPU_CMD_OP_draw, ... and the count. */
#define GPU__CMD_INDEX(name, form, ret, params, args) GPU_CMD_OP_##name,
enum { GPU_CMD_OPS(GPU__CMD_INDEX) GPU_CMD_OP_COUNT };
#undef GPU__CMD_INDEX

/* A backend's table, from the gpu_cmd_<name> functions in scope:
 *   static const gpu_cmd_ops gpu_d3d12_cmd_ops = GPU_CMD_OPS_TABLE; */
#define GPU__CMD_INIT(name, form, ret, params, args) gpu_cmd_##name,
#define GPU_CMD_OPS_TABLE { GPU_CMD_OPS(GPU__CMD_INIT) }

/* The table a recorder carries (its gpu_cmd_head). */
#define GPU_CMD_OPS_OF(c) (((const gpu_cmd_head*)(const void*)(c))->ops)

/* "GPU_CMD_CHECK(tag);" at file scope, after the header that defines the
 * commands (gpu_vt_cmd.h, or a backend's), fails to compile unless every
 * command is there as gpu_cmd_<name>, taking exactly the list's parameters,
 * and every per-draw device call as gpu_fast_<name>, taking those of its
 * gpu_vt function. tag names the check, so one file can hold several. */
#if GPU_CPP11
#define GPU__CMD_CHECK_ONE(name, form, ret, params, args) \
    static_assert(std::is_same<decltype(&gpu_cmd_##name), ret (*) params>::value, \
                  "gpu_cmd_" #name " does not match gpu_cmd_ops.h");
#define GPU__FAST_CHECK_ONE(name, form, ret, params, args) \
    static_assert(std::is_same<decltype(&gpu_fast_##name), ret (*) params>::value && \
                  std::is_same<decltype(gpu_vt::name), ret (*) params>::value, \
                  "gpu_fast_" #name " does not match gpu_cmd_ops.h and gpu_vt");
#define GPU_CMD_CHECK(tag) GPU_CMD_OPS(GPU__CMD_CHECK_ONE) GPU_FAST_FUNCS(GPU__FAST_CHECK_ONE) \
    typedef int gpu_cmd_checked_##tag
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define GPU__CMD_CHECK_ONE(name, form, ret, params, args) \
    _Static_assert(_Generic((&gpu_cmd_##name), ret (*) params: 1, default: 0), \
                   "gpu_cmd_" #name " does not match gpu_cmd_ops.h");
#define GPU__FAST_CHECK_ONE(name, form, ret, params, args) \
    _Static_assert(_Generic((&gpu_fast_##name), ret (*) params: 1, default: 0) && \
                   _Generic((((gpu_vt*)0)->name), ret (*) params: 1, default: 0), \
                   "gpu_fast_" #name " does not match gpu_cmd_ops.h and gpu_vt");
#define GPU_CMD_CHECK(tag) GPU_CMD_OPS(GPU__CMD_CHECK_ONE) GPU_FAST_FUNCS(GPU__FAST_CHECK_ONE) \
    typedef int gpu_cmd_checked_##tag
#else
/* C99: a pointer of the list's type initialised from each function, which a
 * mismatch makes a constraint violation (an error with -pedantic-errors, and
 * by default in current GCC and Clang). */
#if defined(__GNUC__) || defined(__clang__)
#define GPU__CMD_CHECK_UNUSED __attribute__((unused))
#else
#define GPU__CMD_CHECK_UNUSED
#endif
#define GPU__CMD_CHECK_ONE(name, form, ret, params, args) \
    { ret (*p_) params = gpu_cmd_##name; (void)p_; }
#define GPU__FAST_CHECK_ONE(name, form, ret, params, args) \
    { ret (*p_) params = gpu_fast_##name; ret (*q_) params = vt_->name; (void)p_; (void)q_; }
#define GPU_CMD_CHECK(tag) \
    static GPU__CMD_CHECK_UNUSED void gpu_cmd_check_##tag(const gpu_vt* vt_) \
        { GPU_CMD_OPS(GPU__CMD_CHECK_ONE) GPU_FAST_FUNCS(GPU__FAST_CHECK_ONE) } \
    typedef int gpu_cmd_checked_##tag
#endif

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_GPU_CMD_OPS_H */
