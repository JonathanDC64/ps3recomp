/*
 * ps3recomp - the portable renderer's GPU layer: the Vulkan backend's commands
 *
 * Every command of gpu_cmd_ops.h as a static inline function onto the
 * recorder's VkCommandBuffer, through the device's VolkDeviceTable, with no
 * lookup beyond an index into the device's arrays. The draw loop includes
 * this file instead of gpu_vt_cmd.h (gpu_cmd_ops.h says how).
 *
 * Where Vulkan differs from D3D12, the commands make up the difference:
 *   - passes are lazy: pass_begin names the targets, and the next draw, clear
 *     or query_begin begins rendering (load and store keep the contents, as
 *     D3D12's targets do); any barrier, copy, resolve or query reset or
 *     resolve ends it. Between two passes of one recording a barrier orders
 *     the targets' writes, as D3D12 orders them;
 *   - bind_cb and bind_raw record dynamic offsets and bind_table records a
 *     set: the draw binds every set with one vkCmdBindDescriptorSets;
 *   - a barrier's old layout is the one recorded for the texture so far
 *     (tex_layout), so a texture used in COMMON (GENERAL) needs no special
 *     case; states map to layouts, stages and accesses with
 *     gpu_vk_state_layout and gpu_vk_state_sync;
 *   - a depth view bound read-only turns depth writes off (dynamic state),
 *     and so does set_depth_write(0);
 *   - topology is dynamic, primitive restart on for strips only;
 *   - clears inside the pass are vkCmdClearAttachments; a view that is not
 *     bound is cleared in a pass of its own.
 *
 * gpu_pipeline.v is the pipeline table's index + 1, gpu_table.v the VkDescriptorSet, and
 * gpu_addr the buffer's handle and an offset (gpu_vk_internal.h). C only.
 */
#ifndef PS3RECOMP_GPU_VK_CMD_H
#define PS3RECOMP_GPU_VK_CMD_H

#include "gpu_vk_internal.h"

#if defined(PS3RECOMP_RSX_HAL) && defined(PS3RECOMP_RSX_HAL_VULKAN)

static inline void gpu_vk__end(gpu_cmd* c)
{
    c->vk->vkCmdEndRendering(c->cb);
    c->rendering = 0;
}

/* Before a draw: the pass open, every set bound with its dynamic offsets. */
static inline void gpu_vk__ready(gpu_cmd* c)
{
    if (!c->rendering) gpu_vk_pass_open(c);
    if (c->dirty) {
        const gpu_vk_layout* const l = c->lay;
        if (c->set0_stale || c->have != l->sets_mask) {
            gpu_vk_binds_slow(c);
        } else {
            c->vk->vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, l->pl, l->first,
                                           l->nsets - l->first, &c->sets[l->first], l->first ? 0u : l->ndyn,
                                           c->dyn);
            c->dirty = 0;
        }
    }
}

/* One attachment of the open pass cleared, rect NULL for all of it. */
static inline void gpu_vk__clear(gpu_cmd* c, uint32_t att, VkImageAspectFlags aspects, const VkClearValue* value,
                                 const gpu_rect* rect)
{
    VkClearAttachment a;
    VkClearRect r;
    a.aspectMask = aspects;
    a.colorAttachment = att;
    a.clearValue = *value;
    r.rect = c->area;
    if (rect) {
        const int32_t x0 = rect->left > c->area.offset.x ? rect->left : c->area.offset.x;
        const int32_t y0 = rect->top > c->area.offset.y ? rect->top : c->area.offset.y;
        const int32_t ax1 = c->area.offset.x + (int32_t)c->area.extent.width;
        const int32_t ay1 = c->area.offset.y + (int32_t)c->area.extent.height;
        const int32_t x1 = rect->right < ax1 ? rect->right : ax1;
        const int32_t y1 = rect->bottom < ay1 ? rect->bottom : ay1;
        if (x1 <= x0 || y1 <= y0) return;
        r.rect.offset.x = x0;
        r.rect.offset.y = y0;
        r.rect.extent.width = (uint32_t)(x1 - x0);
        r.rect.extent.height = (uint32_t)(y1 - y0);
    }
    r.baseArrayLayer = 0;
    r.layerCount = 1;
    c->vk->vkCmdClearAttachments(c->cb, 1, &a, 1, &r);
}

static inline void gpu_cmd_pass_begin(gpu_cmd* c, const gpu_pass* pass)
{
    if (c->rendering) gpu_vk__end(c);
    c->pass = *pass;
    if (c->pass.nrt > 4) c->pass.nrt = 4;
}

static inline void gpu_cmd_pass_end(gpu_cmd* c)
{
    if (c->rendering) gpu_vk__end(c);
}

static inline void gpu_cmd_clear_color(gpu_cmd* c, gpu_view rtv, const float rgba[4], const gpu_rect* rect)
{
    VkClearValue v;
    memcpy(v.color.float32, rgba, sizeof v.color.float32);
    for (uint32_t i = 0; i < c->pass.nrt; i++)
        if (c->pass.rt[i].id == rtv.id) {
            if (!c->rendering) gpu_vk_pass_open(c);
            gpu_vk__clear(c, i, VK_IMAGE_ASPECT_COLOR_BIT, &v, rect);
            return;
        }
    gpu_vk_clear_unbound(c, rtv, &v, VK_IMAGE_ASPECT_COLOR_BIT, rect);
}

static inline void gpu_cmd_clear_depth(gpu_cmd* c, gpu_view dsv, uint32_t flags, float depth, uint8_t stencil,
                                       const gpu_rect* rect)
{
    VkClearValue v;
    VkImageAspectFlags a = 0;
    v.depthStencil.depth = depth;
    v.depthStencil.stencil = stencil;
    if (flags & GPU_CLEAR_DEPTH) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (flags & GPU_CLEAR_STENCIL) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
    a &= c->view[GPU_HANDLE_INDEX(dsv.id)].aspect;
    if (!a) return;
    if (GPU_VALID(c->pass.ds) && c->pass.ds.id == dsv.id) {
        if (!c->rendering) gpu_vk_pass_open(c);
        gpu_vk__clear(c, 0, a, &v, rect);
        return;
    }
    gpu_vk_clear_unbound(c, dsv, &v, a, rect);
}

/* As SetGraphicsRootSignature, it forgets what the slots held. */
static inline void gpu_cmd_set_layout(gpu_cmd* c, gpu_layout layout)
{
    const gpu_vk_layout* const l = &c->layouts[GPU_HANDLE_INDEX(layout.id)];
    c->lay = l;
    memset(&c->key, 0, sizeof c->key);
    c->key.layout = l->index;
    c->sets[0] = l->set0_fixed;
    c->have = l->set0_fixed ? 1u : 0u;
    c->set0_stale = l->ndyn != 0;
    c->dirty = 1;
}

/* Depth writes are dynamic in the pipelines that write depth (else off in
 * the pipeline, which must then not be given the value), so this one value
 * is theirs: off while the depth view is read-only or set_depth_write said
 * so. */
static inline void gpu_cmd_set_pipeline(gpu_cmd* c, gpu_pipeline pipeline)
{
    const gpu_vk_pso* const p = &c->pso[pipeline.v - 1u];
    c->vk->vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->p);
    c->pipe_dw = p->dyn_dw;
    if (p->dyn_dw) c->vk->vkCmdSetDepthWriteEnable(c->cb, (c->dw && !c->ro_depth) ? VK_TRUE : VK_FALSE);
}

/* GPU_TOPO_* is D3D_PRIMITIVE_TOPOLOGY, one more than VkPrimitiveTopology's
 * number for each of the five. Restart (the strip cut) only on strips: a
 * list's all-ones index is not a cut on D3D12 either. */
static inline void gpu_cmd_set_topology(gpu_cmd* c, uint32_t topology)
{
    const uint32_t strip = topology == GPU_TOPO_LINESTRIP || topology == GPU_TOPO_TRIANGLESTRIP;
    c->vk->vkCmdSetPrimitiveTopology(c->cb, (VkPrimitiveTopology)(topology - 1u));
    if (strip != c->restart) {
        c->restart = strip;
        c->vk->vkCmdSetPrimitiveRestartEnable(c->cb, strip ? VK_TRUE : VK_FALSE);
    }
}

static inline void gpu_cmd_set_depth_write(gpu_cmd* c, uint32_t on)
{
    c->dw = on ? 1u : 0u;
    if (c->pipe_dw) c->vk->vkCmdSetDepthWriteEnable(c->cb, (c->dw && !c->ro_depth) ? VK_TRUE : VK_FALSE);
}

/* gpu_viewport is VkViewport, field for field. The SPIR-V target flips Y in
 * the vertex shader, so D3D12's viewport is Vulkan's as it is. */
static inline void gpu_cmd_set_viewport(gpu_cmd* c, const gpu_viewport* vp)
{
    c->vk->vkCmdSetViewport(c->cb, 0, 1, (const VkViewport*)(const void*)vp);
}

static inline void gpu_cmd_set_scissor(gpu_cmd* c, const gpu_rect* rect)
{
    VkRect2D r;
    r.offset.x = rect->left > 0 ? rect->left : 0;
    r.offset.y = rect->top > 0 ? rect->top : 0;
    r.extent.width = rect->right > r.offset.x ? (uint32_t)(rect->right - r.offset.x) : 0u;
    r.extent.height = rect->bottom > r.offset.y ? (uint32_t)(rect->bottom - r.offset.y) : 0u;
    c->vk->vkCmdSetScissor(c->cb, 0, 1, &r);
}

/* Sets carry their pool: nothing to bind. */
static inline void gpu_cmd_set_table_pools(gpu_cmd* c, gpu_table_pool tex, gpu_table_pool samplers)
{
    (void)c; (void)tex; (void)samplers;
}

/* The address's offset becomes the dynamic offset; a buffer other than the
 * one set 0 holds names another set (found at the draw). */
static inline void gpu_cmd_bind_cb(gpu_cmd* c, uint32_t slot, gpu_addr addr)
{
    const uint32_t k = c->lay->slot[slot].dyn;
    const uint32_t id = GPU_VK_ADDR_ID(addr);
    c->dyn[k] = GPU_VK_ADDR_OFF(addr);
    if (c->key.id[k] != id) {
        c->key.id[k] = id;
        c->set0_stale = 1;
    }
    c->dirty = 1;
}

/* A raw buffer runs from the address to the buffer's end: its offset is the
 * set's, so another offset is another set (today's two parity halves). */
static inline void gpu_cmd_bind_raw(gpu_cmd* c, uint32_t slot, gpu_addr addr)
{
    const uint32_t k = c->lay->slot[slot].dyn;
    const uint32_t id = GPU_VK_ADDR_ID(addr), off = GPU_VK_ADDR_OFF(addr);
    c->dyn[k] = 0;
    if (c->key.id[k] != id || c->key.raw_off[k] != off) {
        c->key.id[k] = id;
        c->key.raw_off[k] = off;
        c->set0_stale = 1;
    }
    c->dirty = 1;
}

static inline void gpu_cmd_bind_table(gpu_cmd* c, uint32_t slot, gpu_table table)
{
    const uint32_t s = c->lay->slot[slot].set;
    c->sets[s] = (VkDescriptorSet)(uintptr_t)table.v;
    c->have |= 1u << s;
    c->dirty = 1;
}

static inline void gpu_cmd_push_const(gpu_cmd* c, uint32_t slot, uint32_t count, const void* values, uint32_t first)
{
    const gpu_vk_layout* const l = c->lay;
    c->vk->vkCmdPushConstants(c->cb, l->pl, l->push_stages, l->slot[slot].push_off + first * 4u, count * 4u, values);
}

static inline void gpu_cmd_push_const1(gpu_cmd* c, uint32_t slot, uint32_t value, uint32_t first)
{
    const gpu_vk_layout* const l = c->lay;
    c->vk->vkCmdPushConstants(c->cb, l->pl, l->push_stages, l->slot[slot].push_off + first * 4u, 4u, &value);
}

/* The stride is the pipeline's (gpu_vertex_layout.stride); the size bounds
 * the reads, as D3D12's view does. */
static inline void gpu_cmd_set_vb(gpu_cmd* c, uint32_t slot, gpu_addr addr, uint32_t size, uint32_t stride)
{
    const VkBuffer b = c->buf[GPU_HANDLE_INDEX(GPU_VK_ADDR_ID(addr))];
    const VkDeviceSize off = GPU_VK_ADDR_OFF(addr), sz = size;
    (void)stride;
    c->vk->vkCmdBindVertexBuffers2(c->cb, slot, 1, &b, &off, &sz, NULL);
}

static inline void gpu_cmd_set_ib(gpu_cmd* c, gpu_addr addr, uint32_t size, gpu_format format)
{
    (void)size;
    c->vk->vkCmdBindIndexBuffer(c->cb, c->buf[GPU_HANDLE_INDEX(GPU_VK_ADDR_ID(addr))], GPU_VK_ADDR_OFF(addr),
                                format == GPU_FORMAT_R16_UINT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

static inline void gpu_cmd_draw(gpu_cmd* c, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                                uint32_t first_instance)
{
    gpu_vk__ready(c);
    c->vk->vkCmdDraw(c->cb, vertices, instances, first_vertex, first_instance);
}

static inline void gpu_cmd_draw_indexed(gpu_cmd* c, uint32_t indices, uint32_t instances, uint32_t first_index,
                                        int32_t base_vertex, uint32_t first_instance)
{
    gpu_vk__ready(c);
    c->vk->vkCmdDrawIndexed(c->cb, indices, instances, first_index, base_vertex, first_instance);
}

/* One texture, every subresource. */
static inline void gpu_cmd_barrier(gpu_cmd* c, gpu_texture texture, gpu_state before, gpu_state after)
{
    const uint32_t i = GPU_HANDLE_INDEX(texture.id);
    const gpu_vk_tex* const t = &c->tex[i];
    VkImageMemoryBarrier2 b;
    VkDependencyInfo di;
    if (c->rendering) gpu_vk__end(c);
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    gpu_vk_state_sync(before, 1, &b.srcStageMask, &b.srcAccessMask);
    gpu_vk_state_sync(after, 0, &b.dstStageMask, &b.dstAccessMask);
    b.oldLayout = (VkImageLayout)c->tex_layout[i];
    b.newLayout = gpu_vk_state_layout(after, t);
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange.aspectMask = t->aspect;
    b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    c->tex_layout[i] = (uint32_t)b.newLayout;
    memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    c->vk->vkCmdPipelineBarrier2(c->cb, &di);
}

static inline void gpu_cmd_barrier_n(gpu_cmd* c, uint32_t n, const gpu_barrier* list)
{
    if (c->rendering) gpu_vk__end(c);
    gpu_vk_rec_barriers(c, n, list);
}

static inline void gpu_cmd_copy_texture(gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, uint32_t x, uint32_t y,
                                        uint32_t z, gpu_texture src, uint32_t src_sub, const gpu_box* box)
{
    if (c->rendering) gpu_vk__end(c);
    gpu_vk_rec_copy_texture(c, dst, dst_sub, x, y, z, src, src_sub, box);
}

static inline void gpu_cmd_copy_texture_whole(gpu_cmd* c, gpu_texture dst, gpu_texture src)
{
    if (c->rendering) gpu_vk__end(c);
    gpu_vk_rec_copy_whole(c, dst, src);
}

static inline void gpu_cmd_copy_buf_to_tex(gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, uint32_t x, uint32_t y,
                                           uint32_t z, gpu_buffer src, const gpu_footprint* from)
{
    if (c->rendering) gpu_vk__end(c);
    gpu_vk_rec_buf_tex(c, 1, dst, dst_sub, x, y, z, src, from, NULL);
}

static inline void gpu_cmd_copy_tex_to_buf(gpu_cmd* c, gpu_buffer dst, const gpu_footprint* to, gpu_texture src,
                                           uint32_t src_sub, const gpu_box* box)
{
    if (c->rendering) gpu_vk__end(c);
    gpu_vk_rec_buf_tex(c, 0, src, src_sub, 0, 0, 0, dst, to, box);
}

static inline void gpu_cmd_copy_buffer(gpu_cmd* c, gpu_buffer dst, uint64_t dst_offset, gpu_buffer src,
                                       uint64_t src_offset, uint64_t size)
{
    VkBufferCopy r;
    if (c->rendering) gpu_vk__end(c);
    r.srcOffset = src_offset;
    r.dstOffset = dst_offset;
    r.size = size;
    c->vk->vkCmdCopyBuffer(c->cb, c->buf[GPU_HANDLE_INDEX(src.id)], c->buf[GPU_HANDLE_INDEX(dst.id)], 1, &r);
}

/* The images' own formats: D3D12's format names the typeless one's reading. */
static inline void gpu_cmd_resolve(gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, gpu_texture src, uint32_t src_sub,
                                   gpu_format format)
{
    (void)format;
    if (c->rendering) gpu_vk__end(c);
    gpu_vk_rec_resolve(c, dst, dst_sub, src, src_sub);
}

static inline void gpu_cmd_query_reset(gpu_cmd* c, gpu_query_pool pool, uint32_t first, uint32_t count)
{
    if (c->rendering) gpu_vk__end(c);
    c->vk->vkCmdResetQueryPool(c->cb, c->qpool[GPU_HANDLE_INDEX(pool.id)].pool, first, count);
}

/* Inside the pass, which a query's draws open: so it opens here. */
static inline void gpu_cmd_query_begin(gpu_cmd* c, gpu_query_pool pool, uint32_t index)
{
    const gpu_vk_qpool* const q = &c->qpool[GPU_HANDLE_INDEX(pool.id)];
    if (!c->rendering) gpu_vk_pass_open(c);
    c->vk->vkCmdBeginQuery(c->cb, q->pool, index, q->precise);
}

static inline void gpu_cmd_query_end(gpu_cmd* c, gpu_query_pool pool, uint32_t index)
{
    c->vk->vkCmdEndQuery(c->cb, c->qpool[GPU_HANDLE_INDEX(pool.id)].pool, index);
}

/* 64-bit results, waited for on the GPU (D3D12's resolve waits too). */
static inline void gpu_cmd_query_resolve(gpu_cmd* c, gpu_query_pool pool, uint32_t first, uint32_t count,
                                         gpu_buffer dst, uint64_t offset)
{
    if (c->rendering) gpu_vk__end(c);
    c->vk->vkCmdCopyQueryPoolResults(c->cb, c->qpool[GPU_HANDLE_INDEX(pool.id)].pool, first, count,
                                     c->buf[GPU_HANDLE_INDEX(dst.id)], offset, sizeof(uint64_t),
                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
}

static inline void gpu_cmd_set_stencil_ref(gpu_cmd* c, uint32_t ref)
{
    c->vk->vkCmdSetStencilReference(c->cb, VK_STENCIL_FACE_FRONT_AND_BACK, ref);
}

static inline void gpu_cmd_set_blend_factor(gpu_cmd* c, const float rgba[4])
{
    c->vk->vkCmdSetBlendConstants(c->cb, rgba);
}

/* A checkpoint (NV) with GPU_DEVICE_BREADCRUMBS, else a debug label when the
 * debug utils are loaded, else nothing. */
static inline void gpu_cmd_marker(gpu_cmd* c, const char* text, uint32_t len)
{
    gpu_vk_rec_marker(c, text, len);
}

/* ---- The per-draw device calls (GPU_FAST_FUNCS) ---- */

/* The memo, as on D3D12: an entry already holding this description is left
 * as it is. srv NULL is today's per-draw empty slot. */
static inline void gpu_fast_table_write_tex(gpu_device* d, gpu_table_pool pool, uint32_t index,
                                            const gpu_view_desc* srv)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_vk_tpool* const p = &dd->tpool[GPU_HANDLE_INDEX(pool.id)];
    gpu_view_desc v;
    if (srv) {
        v = *srv;
    } else {
        memset(&v, 0, sizeof v);
        v.mips = 1;
        v.format = GPU_FORMAT_R8G8B8A8_UNORM;
    }
    if (!v.swizzle) v.swizzle = GPU_SWIZZLE_IDENTITY;
    if (!memcmp(&((const gpu_view_desc*)p->memo)[index], &v, sizeof v)) return;
    gpu_vk_write_tex(dd, p, index, &v);
}

static inline void gpu_fast_table_write_sampler(gpu_device* d, gpu_table_pool pool, uint32_t index, gpu_sampler s)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_vk_tpool* const p = &dd->tpool[GPU_HANDLE_INDEX(pool.id)];
    if (((const uint32_t*)p->memo)[index] == s.id) return;
    gpu_vk_write_sampler(dd, p, index, s);
}

/* The set of the table starting at index (0 when none starts there). */
static inline gpu_table gpu_fast_table_ref(gpu_device* d, gpu_table_pool pool, uint32_t index)
{
    gpu_table t;
    t.v = (uint64_t)(uintptr_t)GPU_VK_DEV(d)->tpool[GPU_HANDLE_INDEX(pool.id)].set_at[index];
    return t;
}

/* No lookup: the handle is the address's high half. */
static inline gpu_addr gpu_fast_buffer_addr(gpu_device* d, gpu_buffer b)
{
    (void)d;
    return GPU_VK_ADDR(b.id);
}

/* (The check is C11's _Static_assert, which clang's -Weverything counts as
 * new to C11; clang before 19 has no such warning.) */
#if defined(__clang__)
#pragma clang diagnostic push
#if __has_warning("-Wpre-c11-compat")
#pragma clang diagnostic ignored "-Wpre-c11-compat"
#endif
#endif
GPU_CMD_CHECK(vk);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#endif /* PS3RECOMP_RSX_HAL && PS3RECOMP_RSX_HAL_VULKAN */
#endif /* PS3RECOMP_GPU_VK_CMD_H */
