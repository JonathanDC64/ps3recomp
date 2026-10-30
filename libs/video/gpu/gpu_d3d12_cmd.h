/*
 * ps3recomp - the portable renderer's GPU layer: the D3D12 backend's commands
 *
 * Every command of gpu_cmd_ops.h as a static inline function, each one call
 * on the recorder's ID3D12GraphicsCommandList (pass_end, set_depth_write and
 * query_reset make none), with no lookup beyond an index into the device's
 * arrays. The draw loop includes this file instead of
 * gpu_vt_cmd.h (gpu_cmd_ops.h says how), so a draw is today's D3D12 calls.
 * The per-draw device calls (GPU_FAST_FUNCS) are here too: the memo compare
 * and the descriptor arithmetic inline, a descriptor write out of line.
 *
 * gpu_pipeline.v is the ID3D12PipelineState*, gpu_table.v the GPU descriptor
 * handle, gpu_addr the GPU virtual address. C only; Windows only for now
 * (vkd3d comes later).
 */
#ifndef PS3RECOMP_GPU_D3D12_CMD_H
#define PS3RECOMP_GPU_D3D12_CMD_H

#include "gpu_d3d12_internal.h"

#if defined(PS3RECOMP_RSX_HAL) && defined(_WIN32)

#include <stddef.h>   /* offsetof */

#define GPU_D3D12_CPU(c, v) { (SIZE_T)(c)->view[GPU_HANDLE_INDEX((v).id)] }

static inline void gpu_cmd_pass_begin(gpu_cmd* c, const gpu_pass* pass)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rt[4];
    D3D12_CPU_DESCRIPTOR_HANDLE ds;
    const UINT n = pass->nrt < 4 ? pass->nrt : 4;
    for (UINT i = 0; i < n; i++) rt[i].ptr = (SIZE_T)c->view[GPU_HANDLE_INDEX(pass->rt[i].id)];
    if (GPU_VALID(pass->ds)) ds.ptr = (SIZE_T)c->view[GPU_HANDLE_INDEX(pass->ds.id)];
    c->cl->lpVtbl->OMSetRenderTargets(c->cl, n, n ? rt : NULL, FALSE, GPU_VALID(pass->ds) ? &ds : NULL);
}

static inline void gpu_cmd_pass_end(gpu_cmd* c)
{
    (void)c;   /* D3D12 has no passes */
}

static inline void gpu_cmd_clear_color(gpu_cmd* c, gpu_view rtv, const float rgba[4], const gpu_rect* rect)
{
    const D3D12_CPU_DESCRIPTOR_HANDLE h = GPU_D3D12_CPU(c, rtv);
    c->cl->lpVtbl->ClearRenderTargetView(c->cl, h, rgba, rect ? 1u : 0u, (const D3D12_RECT*)rect);
}

static inline void gpu_cmd_clear_depth(gpu_cmd* c, gpu_view dsv, uint32_t flags, float depth, uint8_t stencil,
                                       const gpu_rect* rect)
{
    const D3D12_CPU_DESCRIPTOR_HANDLE h = GPU_D3D12_CPU(c, dsv);
    c->cl->lpVtbl->ClearDepthStencilView(c->cl, h, (D3D12_CLEAR_FLAGS)flags, depth, stencil,
                                         rect ? 1u : 0u, (const D3D12_RECT*)rect);
}

static inline void gpu_cmd_set_layout(gpu_cmd* c, gpu_layout layout)
{
    c->cl->lpVtbl->SetGraphicsRootSignature(c->cl, c->layout[GPU_HANDLE_INDEX(layout.id)]);
}

static inline void gpu_cmd_set_pipeline(gpu_cmd* c, gpu_pipeline pipeline)
{
    c->cl->lpVtbl->SetPipelineState(c->cl, (ID3D12PipelineState*)(uintptr_t)pipeline.v);
}

static inline void gpu_cmd_set_topology(gpu_cmd* c, uint32_t topology)
{
    c->cl->lpVtbl->IASetPrimitiveTopology(c->cl, (D3D12_PRIMITIVE_TOPOLOGY)topology);
}

static inline void gpu_cmd_set_depth_write(gpu_cmd* c, uint32_t on)
{
    (void)c; (void)on;   /* a read-only depth view does it on D3D12 */
}

static inline void gpu_cmd_set_viewport(gpu_cmd* c, const gpu_viewport* vp)
{
    c->cl->lpVtbl->RSSetViewports(c->cl, 1, (const D3D12_VIEWPORT*)vp);
}

static inline void gpu_cmd_set_scissor(gpu_cmd* c, const gpu_rect* rect)
{
    c->cl->lpVtbl->RSSetScissorRects(c->cl, 1, (const D3D12_RECT*)rect);
}

/* In today's order: the texture heap, then the sampler heap. */
static inline void gpu_cmd_set_table_pools(gpu_cmd* c, gpu_table_pool tex, gpu_table_pool samplers)
{
    ID3D12DescriptorHeap* h[2];
    UINT n = 0;
    if (GPU_VALID(tex)) h[n++] = c->tpool[GPU_HANDLE_INDEX(tex.id)].heap;
    if (GPU_VALID(samplers)) h[n++] = c->tpool[GPU_HANDLE_INDEX(samplers.id)].heap;
    c->cl->lpVtbl->SetDescriptorHeaps(c->cl, n, h);
}

static inline void gpu_cmd_bind_cb(gpu_cmd* c, uint32_t slot, gpu_addr addr)
{
    c->cl->lpVtbl->SetGraphicsRootConstantBufferView(c->cl, slot, (D3D12_GPU_VIRTUAL_ADDRESS)addr);
}

static inline void gpu_cmd_bind_raw(gpu_cmd* c, uint32_t slot, gpu_addr addr)
{
    c->cl->lpVtbl->SetGraphicsRootShaderResourceView(c->cl, slot, (D3D12_GPU_VIRTUAL_ADDRESS)addr);
}

static inline void gpu_cmd_bind_table(gpu_cmd* c, uint32_t slot, gpu_table table)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    h.ptr = table.v;
    c->cl->lpVtbl->SetGraphicsRootDescriptorTable(c->cl, slot, h);
}

static inline void gpu_cmd_push_const(gpu_cmd* c, uint32_t slot, uint32_t count, const void* values, uint32_t first)
{
    c->cl->lpVtbl->SetGraphicsRoot32BitConstants(c->cl, slot, count, values, first);
}

static inline void gpu_cmd_push_const1(gpu_cmd* c, uint32_t slot, uint32_t value, uint32_t first)
{
    c->cl->lpVtbl->SetGraphicsRoot32BitConstant(c->cl, slot, value, first);
}

static inline void gpu_cmd_set_vb(gpu_cmd* c, uint32_t slot, gpu_addr addr, uint32_t size, uint32_t stride)
{
    D3D12_VERTEX_BUFFER_VIEW v;
    v.BufferLocation = (D3D12_GPU_VIRTUAL_ADDRESS)addr;
    v.SizeInBytes = size;
    v.StrideInBytes = stride;
    c->cl->lpVtbl->IASetVertexBuffers(c->cl, slot, 1, &v);
}

static inline void gpu_cmd_set_ib(gpu_cmd* c, gpu_addr addr, uint32_t size, gpu_format format)
{
    D3D12_INDEX_BUFFER_VIEW v;
    v.BufferLocation = (D3D12_GPU_VIRTUAL_ADDRESS)addr;
    v.SizeInBytes = size;
    v.Format = (DXGI_FORMAT)format;
    c->cl->lpVtbl->IASetIndexBuffer(c->cl, &v);
}

static inline void gpu_cmd_draw(gpu_cmd* c, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                                uint32_t first_instance)
{
    c->cl->lpVtbl->DrawInstanced(c->cl, vertices, instances, first_vertex, first_instance);
}

static inline void gpu_cmd_draw_indexed(gpu_cmd* c, uint32_t indices, uint32_t instances, uint32_t first_index,
                                        int32_t base_vertex, uint32_t first_instance)
{
    c->cl->lpVtbl->DrawIndexedInstanced(c->cl, indices, instances, first_index, base_vertex, first_instance);
}

static inline void gpu_cmd_barrier(gpu_cmd* c, gpu_texture texture, gpu_state before, gpu_state after)
{
    D3D12_RESOURCE_BARRIER b;
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = c->tex[GPU_HANDLE_INDEX(texture.id)];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = (D3D12_RESOURCE_STATES)before;
    b.Transition.StateAfter = (D3D12_RESOURCE_STATES)after;
    c->cl->lpVtbl->ResourceBarrier(c->cl, 1, &b);
}

/* One ResourceBarrier for up to 16, in the list's order (today's lists hold
 * one or two). An aliasing barrier names the texture taking the memory. */
static inline void gpu_cmd_barrier_n(gpu_cmd* c, uint32_t n, const gpu_barrier* list)
{
    D3D12_RESOURCE_BARRIER b[16];
    while (n) {
        const uint32_t k = n < 16 ? n : 16;
        for (uint32_t i = 0; i < k; i++) {
            const gpu_barrier* s = &list[i];
            ID3D12Resource* const r = GPU_VALID(s->texture) ? c->tex[GPU_HANDLE_INDEX(s->texture.id)]
                                    : GPU_VALID(s->buffer) ? c->buf[GPU_HANDLE_INDEX(s->buffer.id)] : NULL;
            memset(&b[i], 0, sizeof b[i]);
            if (s->type == GPU_BARRIER_ALIASING) {
                b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
                b[i].Aliasing.pResourceAfter = r;
            } else {
                b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b[i].Transition.pResource = r;
                b[i].Transition.Subresource = s->sub;
                b[i].Transition.StateBefore = (D3D12_RESOURCE_STATES)s->before;
                b[i].Transition.StateAfter = (D3D12_RESOURCE_STATES)s->after;
            }
        }
        c->cl->lpVtbl->ResourceBarrier(c->cl, k, b);
        list += k;
        n -= k;
    }
}

static inline void gpu_cmd_copy_texture(gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, uint32_t x, uint32_t y,
                                        uint32_t z, gpu_texture src, uint32_t src_sub, const gpu_box* box)
{
    D3D12_TEXTURE_COPY_LOCATION d, s;
    d.pResource = c->tex[GPU_HANDLE_INDEX(dst.id)];
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d.SubresourceIndex = dst_sub;
    s.pResource = c->tex[GPU_HANDLE_INDEX(src.id)];
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.SubresourceIndex = src_sub;
    c->cl->lpVtbl->CopyTextureRegion(c->cl, &d, x, y, z, &s, (const D3D12_BOX*)box);
}

static inline void gpu_cmd_copy_texture_whole(gpu_cmd* c, gpu_texture dst, gpu_texture src)
{
    c->cl->lpVtbl->CopyResource(c->cl, c->tex[GPU_HANDLE_INDEX(dst.id)], c->tex[GPU_HANDLE_INDEX(src.id)]);
}

/* gpu_footprint is D3D12_PLACED_SUBRESOURCE_FOOTPRINT field for field (the
 * contract test checks it). */
static inline void gpu_cmd_copy_buf_to_tex(gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, uint32_t x, uint32_t y,
                                           uint32_t z, gpu_buffer src, const gpu_footprint* from)
{
    D3D12_TEXTURE_COPY_LOCATION d, s;
    d.pResource = c->tex[GPU_HANDLE_INDEX(dst.id)];
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d.SubresourceIndex = dst_sub;
    s.pResource = c->buf[GPU_HANDLE_INDEX(src.id)];
    s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    memcpy(&s.PlacedFootprint, from, sizeof s.PlacedFootprint);
    c->cl->lpVtbl->CopyTextureRegion(c->cl, &d, x, y, z, &s, NULL);
}

static inline void gpu_cmd_copy_tex_to_buf(gpu_cmd* c, gpu_buffer dst, const gpu_footprint* to, gpu_texture src,
                                           uint32_t src_sub, const gpu_box* box)
{
    D3D12_TEXTURE_COPY_LOCATION d, s;
    d.pResource = c->buf[GPU_HANDLE_INDEX(dst.id)];
    d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    memcpy(&d.PlacedFootprint, to, sizeof d.PlacedFootprint);
    s.pResource = c->tex[GPU_HANDLE_INDEX(src.id)];
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.SubresourceIndex = src_sub;
    c->cl->lpVtbl->CopyTextureRegion(c->cl, &d, 0, 0, 0, &s, (const D3D12_BOX*)box);
}

static inline void gpu_cmd_copy_buffer(gpu_cmd* c, gpu_buffer dst, uint64_t dst_offset, gpu_buffer src,
                                       uint64_t src_offset, uint64_t size)
{
    c->cl->lpVtbl->CopyBufferRegion(c->cl, c->buf[GPU_HANDLE_INDEX(dst.id)], dst_offset,
                                    c->buf[GPU_HANDLE_INDEX(src.id)], src_offset, size);
}

static inline void gpu_cmd_resolve(gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, gpu_texture src, uint32_t src_sub,
                                   gpu_format format)
{
    c->cl->lpVtbl->ResolveSubresource(c->cl, c->tex[GPU_HANDLE_INDEX(dst.id)], dst_sub,
                                      c->tex[GPU_HANDLE_INDEX(src.id)], src_sub, (DXGI_FORMAT)format);
}

static inline void gpu_cmd_query_reset(gpu_cmd* c, gpu_query_pool pool, uint32_t first, uint32_t count)
{
    (void)c; (void)pool; (void)first; (void)count;   /* D3D12's queries need none */
}

static inline void gpu_cmd_query_begin(gpu_cmd* c, gpu_query_pool pool, uint32_t index)
{
    const gpu_d3d12_qpool* q = &c->qpool[GPU_HANDLE_INDEX(pool.id)];
    c->cl->lpVtbl->BeginQuery(c->cl, q->heap, q->type, index);
}

static inline void gpu_cmd_query_end(gpu_cmd* c, gpu_query_pool pool, uint32_t index)
{
    const gpu_d3d12_qpool* q = &c->qpool[GPU_HANDLE_INDEX(pool.id)];
    c->cl->lpVtbl->EndQuery(c->cl, q->heap, q->type, index);
}

static inline void gpu_cmd_query_resolve(gpu_cmd* c, gpu_query_pool pool, uint32_t first, uint32_t count,
                                         gpu_buffer dst, uint64_t offset)
{
    const gpu_d3d12_qpool* q = &c->qpool[GPU_HANDLE_INDEX(pool.id)];
    c->cl->lpVtbl->ResolveQueryData(c->cl, q->heap, q->type, first, count, c->buf[GPU_HANDLE_INDEX(dst.id)],
                                    offset);
}

static inline void gpu_cmd_set_stencil_ref(gpu_cmd* c, uint32_t ref)
{
    c->cl->lpVtbl->OMSetStencilRef(c->cl, ref);
}

static inline void gpu_cmd_set_blend_factor(gpu_cmd* c, const float rgba[4])
{
    c->cl->lpVtbl->OMSetBlendFactor(c->cl, rgba);
}

/* DRED keeps a marker's text in the form WinPixEventRuntime gives SetMarker
 * (metadata 2), not a plain string: a SetMarker event with no arguments
 * (type 8 at bit 10, no timestamp), a colour, then the text as an ANSI
 * string (chunk size 8 at bit 55, ANSI at bit 54) packed eight bytes a word,
 * its zero byte included. Today's dred_mark; the text is cut at 255 bytes as
 * its buffer cuts it. */
static inline void gpu_cmd_marker(gpu_cmd* c, const char* text, uint32_t len)
{
    uint64_t q[3 + 256 / 8 + 1];
    const uint32_t n = len < 255 ? len : 255;
    memset(q, 0, sizeof q);
    q[0] = 0x008ull << 10;
    q[1] = 0xFF4080C0ull;
    q[2] = (8ull << 55) | (1ull << 54);
    memcpy(&q[3], text, n);
    const uint32_t words = 3 + (n + 1 + 7) / 8;
    c->cl->lpVtbl->SetMarker(c->cl, 2, q, words * 8u);
}

/* ---- The per-draw device calls (GPU_FAST_FUNCS) ---- */

/* The memo (today's SrvMemo): an entry already holding this description is
 * left as it is. srv NULL is today's per-draw empty slot.
 * The compare reads srv's fields as they are (inline, the caller's values in
 * registers) and packs them as the entry's 20 bytes lie: three words, read
 * from the memo with three loads. A copy of srv built on the stack with
 * narrow stores and read back with a wide load stalled store forwarding on
 * every call; the copy is now made only for a write. */
typedef char gpu_d3d12_memo_layout_[(sizeof(gpu_view_desc) == 20 && offsetof(gpu_view_desc, kind) == 4 &&
                                     offsetof(gpu_view_desc, dim) == 5 && offsetof(gpu_view_desc, flags) == 6 &&
                                     offsetof(gpu_view_desc, mips) == 7 && offsetof(gpu_view_desc, format) == 8 &&
                                     offsetof(gpu_view_desc, swizzle) == 12 && offsetof(gpu_view_desc, mip) == 16 &&
                                     offsetof(gpu_view_desc, layer) == 18) ? 1 : -1];
static inline void gpu_fast_table_write_tex(gpu_device* d, gpu_table_pool pool, uint32_t index,
                                            const gpu_view_desc* srv)
{
    static const gpu_view_desc none = { {0}, GPU_VIEW_SRV, GPU_DIM_2D, 0, 1, GPU_FORMAT_R8G8B8A8_UNORM, 0, 0, 0 };
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    gpu_d3d12_tpool* const p = &dd->tpool[GPU_HANDLE_INDEX(pool.id)];
    const gpu_view_desc* const s = srv ? srv : &none;
    const uint32_t swizzle = s->swizzle ? s->swizzle : GPU_SWIZZLE_IDENTITY;
    /* Little-endian (every target of this backend). */
    const uint64_t w0 = (uint64_t)s->texture.id | (uint64_t)s->kind << 32 | (uint64_t)s->dim << 40 |
                        (uint64_t)s->flags << 48 | (uint64_t)s->mips << 56;
    const uint64_t w1 = (uint64_t)s->format | (uint64_t)swizzle << 32;
    const uint32_t w2 = (uint32_t)s->mip | (uint32_t)s->layer << 16;
    const gpu_view_desc* const e = &((const gpu_view_desc*)p->memo)[index];
    uint64_t m0, m1;
    uint32_t m2;
    gpu_view_desc v;
    memcpy(&m0, e, 8);             /* texture, kind, dim, flags, mips */
    memcpy(&m1, &e->format, 8);    /* format, swizzle */
    memcpy(&m2, &e->mip, 4);       /* mip, layer */
    if (!((m0 ^ w0) | (m1 ^ w1) | (uint64_t)(m2 ^ w2))) return;
    v = *s;
    v.swizzle = swizzle;
    gpu_d3d12_write_srv(dd, p, index, &v);
}

static inline void gpu_fast_table_write_sampler(gpu_device* d, gpu_table_pool pool, uint32_t index, gpu_sampler s)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    gpu_d3d12_tpool* const p = &dd->tpool[GPU_HANDLE_INDEX(pool.id)];
    if (((const uint32_t*)p->memo)[index] == s.id) return;
    gpu_d3d12_write_sampler(dd, p, index, s);
}

/* Today's GetGPUDescriptorHandleForHeapStart + index * increment. */
static inline gpu_table gpu_fast_table_ref(gpu_device* d, gpu_table_pool pool, uint32_t index)
{
    const gpu_d3d12_tpool* const p = &GPU_D3D12_DEV(d)->tpool[GPU_HANDLE_INDEX(pool.id)];
    gpu_table t;
    t.v = p->gpu + (uint64_t)index * p->inc;
    return t;
}

static inline gpu_addr gpu_fast_buffer_addr(gpu_device* d, gpu_buffer b)
{
    return GPU_D3D12_DEV(d)->buf_va[GPU_HANDLE_INDEX(b.id)];
}

/* (The check is C11's _Static_assert, which clang's -Weverything counts as
 * new to C11.) */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpre-c11-compat"
#endif
GPU_CMD_CHECK(d3d12);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#endif /* PS3RECOMP_RSX_HAL && _WIN32 */
#endif /* PS3RECOMP_GPU_D3D12_CMD_H */
