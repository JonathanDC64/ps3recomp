/*
 * ps3recomp - the portable renderer's GPU layer: the Vulkan backend's descriptors
 *
 * Views (a VkImageView per description, its swizzle a VkComponentMapping),
 * samplers (one per description), table pools (a descriptor set per table,
 * made with the pool, so a table is a VkDescriptorSet and bind_table binds
 * it as it is), and set 0's sets (the layout's dynamic buffers, one set per
 * buffers bound, found again only when they change).
 *
 * An empty entry is a null descriptor (robustness2), or without them a 1x1
 * transparent black texture of the entry's dimension.
 *
 * Built with PS3RECOMP_RSX_HAL and PS3RECOMP_RSX_HAL_VULKAN.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(PS3RECOMP_RSX_HAL_VULKAN)

#include "gpu_vk_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Views
 * -------------------------------------------------------------------------*/

/* Shader4ComponentMapping onto the format's own channels: each output reads
 * a DXGI channel (wherever the image keeps it) or a forced 0 or 1. */
static VkComponentSwizzle swz_of(const gpu_vk_format* f, uint32_t swizzle, uint32_t c)
{
    const uint32_t sel = GPU_SWIZZLE_GET(swizzle, c);
    if (sel <= GPU_SWZ_A) return (VkComponentSwizzle)f->swz[sel];
    return sel == GPU_SWZ_ZERO ? VK_COMPONENT_SWIZZLE_ZERO : VK_COMPONENT_SWIZZLE_ONE;
}

/* Under lock: a new view of the description's texture; index + 1, or 0. */
static uint32_t view_make(gpu_vk_device* dd, const gpu_view_desc* v, uint32_t kind)
{
    const uint32_t ti = GPU_HANDLE_INDEX(v->texture.id);
    gpu_vk_tex* const t = &dd->tex[ti];
    const gpu_vk_format* vf = gpu_vk_format_of(v->format ? v->format : t->format, t->use);
    if (!vf) vf = gpu_vk_format_of(v->format ? v->format : t->format, GPU_USE_DS);
    if (!vf) return 0;
    VkImageViewCreateInfo vi;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t->image;
    vi.format = vf->vk;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (t->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        /* A depth texture's views are its own format; which plane a texture
         * view reads is its format's (R24_UNORM_X8 depth, X24_G8 stencil). */
        vi.format = t->vkfmt;
        if (kind == GPU_VIEW_RTV) return 0;
        if (kind == GPU_VIEW_DSV) aspect = t->aspect;
        else if (vf->aspect == GPU_VK_ASPECT_STENCIL) aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
        else aspect = (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
    } else if (kind == GPU_VIEW_DSV) {
        return 0;
    }
    vi.subresourceRange.aspectMask = aspect;
    if (kind == GPU_VIEW_SRV) {
        const uint32_t sw = v->swizzle ? v->swizzle : GPU_SWIZZLE_IDENTITY;
        vi.components.r = swz_of(vf, sw, 0);
        vi.components.g = swz_of(vf, sw, 1);
        vi.components.b = swz_of(vf, sw, 2);
        vi.components.a = swz_of(vf, sw, 3);
        vi.subresourceRange.baseMipLevel = v->mip;
        vi.subresourceRange.levelCount = v->mips ? v->mips : VK_REMAINING_MIP_LEVELS;
        switch (v->dim) {
        case GPU_DIM_CUBE:
            vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
            vi.subresourceRange.layerCount = 6;
            break;
        case GPU_DIM_2D_ARRAY:
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            vi.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
            break;
        default:   /* 2D and 2DMS: the first layer */
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.subresourceRange.layerCount = 1;
            break;
        }
    } else {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange.baseMipLevel = v->mip;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.baseArrayLayer = v->layer;
        vi.subresourceRange.layerCount = 1;
    }
    VkImageView iv = VK_NULL_HANDLE;
    if (dd->vk.vkCreateImageView(dd->device, &vi, NULL, &iv) != VK_SUCCESS) return 0;
    const uint32_t id = gpu_vk_slot_alloc(&dd->view_slots);
    if (!id) {
        dd->vk.vkDestroyImageView(dd->device, iv, NULL);
        return 0;
    }
    const uint32_t j = GPU_HANDLE_INDEX(id);
    gpu_vk_view* const w = &dd->view[j];
    w->desc = *v;
    w->iv = iv;
    w->next = t->views;
    w->tex = ti;
    w->width = t->width >> v->mip ? t->width >> v->mip : 1u;
    w->height = t->height >> v->mip ? t->height >> v->mip : 1u;
    w->aspect = aspect;
    t->views = j + 1;
    return j + 1;
}

/* Under lock: the texture's view of this description, made the first time. */
uint32_t gpu_vk_view_find(gpu_vk_device* dd, const gpu_view_desc* desc, uint32_t kind)
{
    const gpu_vk_tex* const t = &dd->tex[GPU_HANDLE_INDEX(desc->texture.id)];
    for (uint32_t v = t->views; v; v = dd->view[v - 1].next)
        if (!memcmp(&dd->view[v - 1].desc, desc, sizeof *desc)) return v;
    return view_make(dd, desc, kind);
}

gpu_view gpu_vk_view_get(gpu_device* d, const gpu_view_desc* desc)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_view r = {0};
    gpu_vk_lock(&dd->lock);
    if (gpu_vk_slot_valid(&dd->tex_slots, desc->texture.id)) {
        const uint32_t v = gpu_vk_view_find(dd, desc, desc->kind);
        if (v) r.id = GPU_HANDLE_MAKE(v - 1, dd->view_slots.gen[v - 1]);
    }
    gpu_vk_unlock(&dd->lock);
    return r;
}

/* ---------------------------------------------------------------------------
 * Samplers
 * -------------------------------------------------------------------------*/

static VkSamplerAddressMode addr_mode(const gpu_vk_device* dd, uint32_t a)
{
    switch (a) {
    case GPU_ADDRESS_WRAP:        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case GPU_ADDRESS_MIRROR:      return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case GPU_ADDRESS_BORDER:      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case GPU_ADDRESS_MIRROR_ONCE:
        /* Without samplerMirrorClampToEdge: mirrored, past one mirror too. */
        return (dd->base.caps.bits & GPU_CAP_MIRROR_ONCE) ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
                                                          : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default:                      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

/* The border colour Vulkan has nearest to the description's, by RPCS3's rule
 * when custom border colours are missing: white when the colour channels add
 * up to more than 1.35, else opaque black when alpha is over half, else
 * transparent black. Exact for those three, which is what the guest uses
 * (the shadow maps' white border, the post passes' defaults). */
static VkBorderColor border_color(const float b[4])
{
    if (b[0] + b[1] + b[2] > 1.35f) return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    return b[3] > 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
}

/* D3D12_SAMPLER_DESC as a VkSampler. Anisotropy without GPU_CAP_ANISO16 is
 * capped at the device's (none without samplerAnisotropy). */
VkSampler gpu_vk_sampler_make(gpu_vk_device* dd, const gpu_sampler_desc* desc)
{
    const uint32_t f = desc->filter;
    VkSamplerCreateInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.minFilter = ((f >> 4) & 3u) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.magFilter = ((f >> 2) & 3u) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = (f & 3u) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = addr_mode(dd, desc->address_u);
    si.addressModeV = addr_mode(dd, desc->address_v);
    si.addressModeW = addr_mode(dd, desc->address_w);
    si.mipLodBias = desc->mip_lod_bias;
    if (GPU_FILTER_IS_ANISO(f) && dd->has_aniso && desc->max_aniso > 1) {
        const uint32_t cap = dd->base.caps.max_aniso;
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = (float)(desc->max_aniso < cap ? desc->max_aniso : cap);
    }
    if (((f >> 7) & 3u) == GPU_REDUCE_COMPARE) {
        si.compareEnable = VK_TRUE;
        si.compareOp = desc->compare ? (VkCompareOp)(desc->compare - 1u) : VK_COMPARE_OP_NEVER;
    }
    si.minLod = desc->min_lod;
    si.maxLod = desc->max_lod >= GPU_LOD_MAX ? VK_LOD_CLAMP_NONE : desc->max_lod;
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    si.borderColor = border_color(desc->border);
    VkSampler s = VK_NULL_HANDLE;
    if (dd->vk.vkCreateSampler(dd->device, &si, NULL, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
    return s;
}

/* Cached by description in an open-addressed table of slot index + 1. */
gpu_sampler gpu_vk_sampler_get(gpu_device* d, const gpu_sampler_desc* desc)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_sampler r = {0};
    const uint32_t h = (uint32_t)gpu_vk_fnv(GPU_VK_FNV_BASIS, desc, sizeof *desc);
    gpu_vk_lock(&dd->lock);
    if (dd->samp_hash) {
        for (uint32_t k = h & (dd->samp_hash_cap - 1);; k = (k + 1) & (dd->samp_hash_cap - 1)) {
            const uint32_t e = dd->samp_hash[k];
            if (!e) break;
            if (!memcmp(&dd->samp[e - 1].desc, desc, sizeof *desc)) {
                r.id = GPU_HANDLE_MAKE(e - 1, dd->samp_slots.gen[e - 1]);
                gpu_vk_unlock(&dd->lock);
                return r;
            }
        }
    }
    /* Grow at half full. */
    if (!dd->samp_hash || (dd->samp_slots.used + 1) * 2 > dd->samp_hash_cap) {
        const uint32_t cap = dd->samp_hash_cap ? dd->samp_hash_cap * 2 : 256;
        uint32_t* n = (uint32_t*)calloc(cap, sizeof *n);
        if (n) {
            for (uint32_t i = 0; i < dd->samp_slots.used; i++)
                if (dd->samp_slots.live[i]) {
                    const uint32_t hh = (uint32_t)gpu_vk_fnv(GPU_VK_FNV_BASIS, &dd->samp[i].desc, sizeof dd->samp[i].desc);
                    uint32_t k = hh & (cap - 1);
                    while (n[k]) k = (k + 1) & (cap - 1);
                    n[k] = i + 1;
                }
            free(dd->samp_hash);
            dd->samp_hash = n;
            dd->samp_hash_cap = cap;
        }
    }
    const VkSampler s = dd->samp_hash ? gpu_vk_sampler_make(dd, desc) : VK_NULL_HANDLE;
    if (s) {
        r.id = gpu_vk_slot_alloc(&dd->samp_slots);
        if (r.id) {
            const uint32_t i = GPU_HANDLE_INDEX(r.id);
            dd->samp[i].desc = *desc;
            dd->samp[i].s = s;
            uint32_t k = h & (dd->samp_hash_cap - 1);
            while (dd->samp_hash[k]) k = (k + 1) & (dd->samp_hash_cap - 1);
            dd->samp_hash[k] = i + 1;
        } else {
            dd->vk.vkDestroySampler(dd->device, s, NULL);
        }
    }
    gpu_vk_unlock(&dd->lock);
    return r;
}

/* ---------------------------------------------------------------------------
 * Table pools
 * -------------------------------------------------------------------------*/

/* Under lock: the set layout of a table of `size` entries, made once. Every
 * entry may be left unwritten and written while the set is in flight, as
 * long as the GPU does not read that entry (D3D12's tables). Step VC1: a
 * short table's layout has 16 entries (gpu_vk_table_entries). */
VkDescriptorSetLayout gpu_vk_table_layout(gpu_vk_device* dd, uint32_t kind, uint32_t size)
{
    if (!size) return VK_NULL_HANDLE;
    size = gpu_vk_table_entries(size);
    for (uint32_t i = 0; i < dd->ntlayout; i++)
        if (dd->tlayout[i].kind == kind && dd->tlayout[i].size == size) return dd->tlayout[i].l;
    if (dd->ntlayout == 64 || !size || size > 64) return VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding b[64];
    VkDescriptorBindingFlags bf[64];
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                      (dd->has_gs ? VK_SHADER_STAGE_GEOMETRY_BIT : 0u);
    memset(b, 0, sizeof b);
    for (uint32_t j = 0; j < size; j++) {
        b[j].binding = (kind == GPU_TABLE_SAMPLER ? GPU_VK_BIND_S : GPU_VK_BIND_T) + j;
        b[j].descriptorType = kind == GPU_TABLE_SAMPLER ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        b[j].descriptorCount = 1;
        b[j].stageFlags = stages;
        bf[j] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi;
    VkDescriptorSetLayoutCreateInfo li;
    memset(&fi, 0, sizeof fi);
    memset(&li, 0, sizeof li);
    fi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    fi.bindingCount = size;
    fi.pBindingFlags = bf;
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.pNext = &fi;
    li.bindingCount = size;
    li.pBindings = b;
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    if (dd->vk.vkCreateDescriptorSetLayout(dd->device, &li, NULL, &l) != VK_SUCCESS) return VK_NULL_HANDLE;
    dd->tlayout[dd->ntlayout].kind = kind;
    dd->tlayout[dd->ntlayout].size = size;
    dd->tlayout[dd->ntlayout].l = l;
    dd->ntlayout++;
    return l;
}

/* The empty entry's image: a null descriptor, or the dummy of the entry's
 * dimension and kind. */
static void empty_info(gpu_vk_device* dd, const gpu_view_desc* v, VkDescriptorImageInfo* ii)
{
    memset(ii, 0, sizeof *ii);
    ii->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (dd->has_null_desc) return;
    const gpu_vk_format* const f = gpu_vk_format_of(v->format, 0);
    const uint32_t dim = v->dim < 4 ? v->dim : 0u, kind = f && f->kind ? 1u : 0u;
    const gpu_texture t = GPU_VALID(dd->dummy[dim][kind]) ? dd->dummy[dim][kind] : dd->dummy[0][kind];
    if (!GPU_VALID(t)) return;
    gpu_view_desc dv;
    memset(&dv, 0, sizeof dv);
    dv.texture = t;
    dv.kind = GPU_VIEW_SRV;
    dv.dim = GPU_VALID(dd->dummy[dim][kind]) ? (uint8_t)dim : (uint8_t)GPU_DIM_2D;
    dv.mips = 1;
    dv.swizzle = GPU_SWIZZLE_IDENTITY;
    gpu_vk_lock(&dd->lock);
    const uint32_t vi = gpu_vk_view_find(dd, &dv, GPU_VIEW_SRV);
    if (vi) ii->imageView = dd->view[vi - 1].iv;
    gpu_vk_unlock(&dd->lock);
}

/* One set per table, every entry empty: a texture pool's entries as
 * R8_UNORM null ones (today's srv heap), a sampler pool's unwritten. */
gpu_table_pool gpu_vk_table_pool_create(gpu_device* d, const gpu_table_pool_desc* desc)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_table_pool r = {0};
    const int samp = desc->kind == GPU_TABLE_SAMPLER;
    if ((desc->kind != GPU_TABLE_TEX && !samp) || !desc->count || !desc->nregions ||
        desc->nregions > GPU_MAX_TABLE_REGIONS)
        return r;
    uint32_t nsets = 0, ndesc = 0;
    for (uint32_t k = 0; k < desc->nregions; k++) {
        const gpu_table_region* const g = &desc->region[k];
        if (!g->tables || !g->size || g->size > 64 || (uint64_t)g->first + (uint64_t)g->tables * g->size > desc->count)
            return r;
        nsets += g->tables;
        ndesc += g->tables * gpu_vk_table_entries(g->size);   /* (step VC1: what each set's layout takes) */
    }
    VkDescriptorSetLayout lay[GPU_MAX_TABLE_REGIONS];
    gpu_vk_lock(&dd->lock);
    for (uint32_t k = 0; k < desc->nregions; k++) lay[k] = gpu_vk_table_layout(dd, desc->kind, desc->region[k].size);
    gpu_vk_unlock(&dd->lock);
    for (uint32_t k = 0; k < desc->nregions; k++)
        if (!lay[k]) return r;

    VkDescriptorPoolSize ps;
    VkDescriptorPoolCreateInfo pi;
    ps.type = samp ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    ps.descriptorCount = ndesc;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = nsets;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (dd->vk.vkCreateDescriptorPool(dd->device, &pi, NULL, &pool) != VK_SUCCESS) return r;
    VkDescriptorSet* sets = (VkDescriptorSet*)calloc(nsets, sizeof *sets);
    VkDescriptorSet* set_at = (VkDescriptorSet*)calloc(desc->count, sizeof *set_at);
    void* memo = calloc(desc->count, samp ? sizeof(uint32_t) : sizeof(gpu_view_desc));
    int ok = sets && set_at && memo;
    uint32_t base[GPU_MAX_TABLE_REGIONS];
    {
        VkDescriptorSetLayout ls[256];
        uint32_t at = 0;
        for (uint32_t k = 0; ok && k < desc->nregions; k++) {
            base[k] = at;
            for (uint32_t i = 0; i < 256; i++) ls[i] = lay[k];
            for (uint32_t t = 0; ok && t < desc->region[k].tables; t += 256) {
                const uint32_t m = desc->region[k].tables - t < 256 ? desc->region[k].tables - t : 256u;
                VkDescriptorSetAllocateInfo ai;
                memset(&ai, 0, sizeof ai);
                ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                ai.descriptorPool = pool;
                ai.descriptorSetCount = m;
                ai.pSetLayouts = ls;
                ok = dd->vk.vkAllocateDescriptorSets(dd->device, &ai, &sets[at]) == VK_SUCCESS;
                at += m;
            }
        }
    }
    if (ok) {
        /* The tables by first entry, the first region's where they overlap. */
        for (uint32_t k = 0; k < desc->nregions; k++)
            for (uint32_t t = 0; t < desc->region[k].tables; t++) {
                const uint32_t e = desc->region[k].first + t * desc->region[k].size;
                if (!set_at[e]) set_at[e] = sets[base[k] + t];
            }
        if (!samp) {
            gpu_view_desc* const m = (gpu_view_desc*)memo;
            gpu_view_desc nv;
            memset(&nv, 0, sizeof nv);
            nv.mips = 1;
            nv.format = GPU_FORMAT_R8_UNORM;
            nv.swizzle = GPU_SWIZZLE_IDENTITY;
            for (uint32_t k = 0; k < desc->count; k++) m[k] = nv;
            VkDescriptorImageInfo ii[64];
            VkDescriptorImageInfo one;
            empty_info(dd, &nv, &one);
            for (uint32_t j = 0; j < 64; j++) ii[j] = one;
            VkWriteDescriptorSet w[256];
            uint32_t nw = 0, at = 0;
            for (uint32_t k = 0; k < desc->nregions; k++)
                for (uint32_t t = 0; t < desc->region[k].tables; t++) {
                    memset(&w[nw], 0, sizeof w[nw]);
                    w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    w[nw].dstSet = sets[at++];
                    w[nw].dstBinding = GPU_VK_BIND_T;
                    w[nw].descriptorCount = desc->region[k].size;
                    w[nw].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    w[nw].pImageInfo = ii;
                    if (++nw == 256) {
                        dd->vk.vkUpdateDescriptorSets(dd->device, nw, w, 0, NULL);
                        nw = 0;
                    }
                }
            if (nw) dd->vk.vkUpdateDescriptorSets(dd->device, nw, w, 0, NULL);
        }
        gpu_vk_lock(&dd->lock);
        r.id = gpu_vk_slot_alloc(&dd->tpool_slots);
        gpu_vk_unlock(&dd->lock);
    }
    if (!r.id) {
        dd->vk.vkDestroyDescriptorPool(dd->device, pool, NULL);
        free(sets);
        free(set_at);
        free(memo);
        return r;
    }
    gpu_vk_tpool* const p = &dd->tpool[GPU_HANDLE_INDEX(r.id)];
    p->kind = desc->kind;
    p->count = desc->count;
    p->desc = *desc;
    p->pool = pool;
    p->sets = sets;
    memcpy(p->region_base, base, sizeof base);
    p->set_at = set_at;
    p->memo = memo;
    return r;
}

/* Every table holding the entry, as (set, entry within it). */
static uint32_t holders(const gpu_vk_tpool* p, uint32_t index, VkDescriptorSet* set, uint32_t* j)
{
    uint32_t n = 0;
    for (uint32_t k = 0; k < p->desc.nregions; k++) {
        const gpu_table_region* const g = &p->desc.region[k];
        if (index < g->first) continue;
        const uint32_t off = index - g->first, t = off / g->size;
        if (t >= g->tables) continue;
        set[n] = p->sets[p->region_base[k] + t];
        j[n] = off % g->size;
        n++;
    }
    return n;
}

/* The memo's miss: the descriptor written into every table holding the
 * entry. The render thread's (the sets are written without a lock). */
void gpu_vk_write_tex(gpu_vk_device* dd, gpu_vk_tpool* p, uint32_t index, const gpu_view_desc* v)
{
    VkDescriptorImageInfo ii;
    if (GPU_VALID(v->texture)) {
        memset(&ii, 0, sizeof ii);
        gpu_vk_lock(&dd->lock);
        if (gpu_vk_slot_valid(&dd->tex_slots, v->texture.id)) {
            const uint32_t vi = gpu_vk_view_find(dd, v, GPU_VIEW_SRV);
            if (vi) {
                ii.imageView = dd->view[vi - 1].iv;
                ii.imageLayout = gpu_vk_read_layout(&dd->tex[GPU_HANDLE_INDEX(v->texture.id)]);
            }
        }
        gpu_vk_unlock(&dd->lock);
        if (!ii.imageView) empty_info(dd, v, &ii);
    } else {
        empty_info(dd, v, &ii);
    }
    VkDescriptorSet set[GPU_MAX_TABLE_REGIONS];
    uint32_t j[GPU_MAX_TABLE_REGIONS];
    VkWriteDescriptorSet w[GPU_MAX_TABLE_REGIONS];
    const uint32_t n = holders(p, index, set, j);
    for (uint32_t k = 0; k < n; k++) {
        memset(&w[k], 0, sizeof w[k]);
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[k].dstSet = set[k];
        w[k].dstBinding = GPU_VK_BIND_T + j[k];
        w[k].descriptorCount = 1;
        w[k].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[k].pImageInfo = &ii;
    }
    if (n) dd->vk.vkUpdateDescriptorSets(dd->device, n, w, 0, NULL);
    ((gpu_view_desc*)p->memo)[index] = *v;
    gpu_vk_add32(&dd->n_srv_writes);
}

void gpu_vk_write_sampler(gpu_vk_device* dd, gpu_vk_tpool* p, uint32_t index, gpu_sampler s)
{
    if (!gpu_vk_slot_valid(&dd->samp_slots, s.id)) return;
    VkDescriptorImageInfo ii;
    memset(&ii, 0, sizeof ii);
    ii.sampler = dd->samp[GPU_HANDLE_INDEX(s.id)].s;
    VkDescriptorSet set[GPU_MAX_TABLE_REGIONS];
    uint32_t j[GPU_MAX_TABLE_REGIONS];
    VkWriteDescriptorSet w[GPU_MAX_TABLE_REGIONS];
    const uint32_t n = holders(p, index, set, j);
    for (uint32_t k = 0; k < n; k++) {
        memset(&w[k], 0, sizeof w[k]);
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[k].dstSet = set[k];
        w[k].dstBinding = GPU_VK_BIND_S + j[k];
        w[k].descriptorCount = 1;
        w[k].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w[k].pImageInfo = &ii;
    }
    if (n) dd->vk.vkUpdateDescriptorSets(dd->device, n, w, 0, NULL);
    ((uint32_t*)p->memo)[index] = s.id;
    gpu_vk_add32(&dd->n_sampler_writes);
}

/* ---------------------------------------------------------------------------
 * Set 0: the layout's dynamic buffers
 * -------------------------------------------------------------------------*/

/* Under lock: a set from the set-0 pools (a new pool when the last is full). */
VkDescriptorSet gpu_vk_set0_alloc(gpu_vk_device* dd, VkDescriptorSetLayout l)
{
    for (int tries = 0; tries < 2; tries++) {
        if (!dd->nset0_pools || !dd->set0_left || tries) {
            VkDescriptorPoolSize ps[3];
            VkDescriptorPoolCreateInfo pi;
            ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            ps[0].descriptorCount = 256 * GPU_VK_MAX_DYN;
            ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            ps[1].descriptorCount = 256 * GPU_VK_MAX_DYN;
            ps[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
            ps[2].descriptorCount = 256 * GPU_MAX_STATIC_SAMPLERS;
            memset(&pi, 0, sizeof pi);
            pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pi.maxSets = 256;
            pi.poolSizeCount = 3;
            pi.pPoolSizes = ps;
            VkDescriptorPool* np = (VkDescriptorPool*)realloc(dd->set0_pools, (dd->nset0_pools + 1) * sizeof *np);
            if (!np) return VK_NULL_HANDLE;
            dd->set0_pools = np;
            if (dd->vk.vkCreateDescriptorPool(dd->device, &pi, NULL, &np[dd->nset0_pools]) != VK_SUCCESS)
                return VK_NULL_HANDLE;
            dd->nset0_pools++;
            dd->set0_left = 256;
        }
        VkDescriptorSetAllocateInfo ai;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = dd->set0_pools[dd->nset0_pools - 1];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &l;
        VkDescriptorSet s = VK_NULL_HANDLE;
        if (dd->vk.vkAllocateDescriptorSets(dd->device, &ai, &s) == VK_SUCCESS) {
            dd->set0_left--;
            return s;
        }
    }
    return VK_NULL_HANDLE;
}

/* The set holding a layout's buffers as bound: a CB is the buffer from 0
 * over one CB range (its address's offset the dynamic offset), a RAW buffer
 * runs from its address to the buffer's end. Found in a cache keyed by the
 * buffers' handles (never reused) and the RAW offsets; NULL while a slot's
 * buffer is not bound. Sets live as long as the device. */
VkDescriptorSet gpu_vk_set0_get(gpu_vk_device* dd, const gpu_vk_layout* l, const gpu_vk_set0_key* key)
{
    for (uint32_t k = 0; k < l->ndyn; k++)
        if (!key->id[k]) return VK_NULL_HANDLE;
    const uint32_t h = (uint32_t)gpu_vk_fnv(GPU_VK_FNV_BASIS, key, sizeof *key);
    VkDescriptorSet s = VK_NULL_HANDLE;
    gpu_vk_lock(&dd->lock);
    dd->n_set0_gets++;
    if (dd->set0_map)
        for (uint32_t k = h & (dd->set0_cap - 1);; k = (k + 1) & (dd->set0_cap - 1)) {
            const gpu_vk_set0_entry* const e = &dd->set0_map[k];
            if (!e->used) break;
            if (!memcmp(&e->key, key, sizeof *key)) {
                s = e->set;
                gpu_vk_unlock(&dd->lock);
                return s;
            }
        }
    if (!dd->set0_map || (dd->set0_n + 1) * 2 > dd->set0_cap) {
        const uint32_t cap = dd->set0_cap ? dd->set0_cap * 2 : 256;
        gpu_vk_set0_entry* n = (gpu_vk_set0_entry*)calloc(cap, sizeof *n);
        if (!n) {
            gpu_vk_unlock(&dd->lock);
            return VK_NULL_HANDLE;
        }
        for (uint32_t i = 0; i < dd->set0_cap; i++)
            if (dd->set0_map[i].used) {
                uint32_t k = (uint32_t)gpu_vk_fnv(GPU_VK_FNV_BASIS, &dd->set0_map[i].key, sizeof(gpu_vk_set0_key)) &
                             (cap - 1);
                while (n[k].used) k = (k + 1) & (cap - 1);
                n[k] = dd->set0_map[i];
            }
        free(dd->set0_map);
        dd->set0_map = n;
        dd->set0_cap = cap;
    }
    VkDescriptorBufferInfo bi[GPU_VK_MAX_DYN];
    VkWriteDescriptorSet w[GPU_VK_MAX_DYN];
    int ok = 1;
    for (uint32_t k = 0; k < l->ndyn; k++) {
        if (!gpu_vk_slot_valid(&dd->buf_slots, key->id[k])) {
            ok = 0;
            break;
        }
        const uint32_t i = GPU_HANDLE_INDEX(key->id[k]);
        const gpu_vk_buf* const b = &dd->buf[i];
        bi[k].buffer = dd->buf_vk[i];
        if (l->dyn_raw[k]) {
            if (key->raw_off[k] >= b->size) {
                ok = 0;
                break;
            }
            uint64_t range = b->size - key->raw_off[k];
            if (range > dd->props.limits.maxStorageBufferRange) {
                /* Step V5v: the fetch's loads (E3) read 0 past the cut, so a
                 * vertex-shader clamp table past it loses its corners: say so. */
                static int said;
                if (!said++)
                    printf("[VK] a raw buffer's range is cut at the device's %u MB (maxStorageBufferRange): "
                           "reads past it give 0\n", (unsigned)(dd->props.limits.maxStorageBufferRange >> 20));
                range = dd->props.limits.maxStorageBufferRange;
            }
            bi[k].offset = key->raw_off[k];
            bi[k].range = range;
        } else {
            bi[k].offset = 0;
            bi[k].range = b->padded || b->size >= dd->cb_range ? dd->cb_range : b->size;
        }
        memset(&w[k], 0, sizeof w[k]);
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[k].dstBinding = l->dyn_binding[k];
        w[k].descriptorCount = 1;
        w[k].descriptorType = l->dyn_raw[k] ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
                                            : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w[k].pBufferInfo = &bi[k];
    }
    if (ok) s = gpu_vk_set0_alloc(dd, l->set0);
    if (s) {
        for (uint32_t k = 0; k < l->ndyn; k++) w[k].dstSet = s;
        dd->vk.vkUpdateDescriptorSets(dd->device, l->ndyn, w, 0, NULL);
        uint32_t k = h & (dd->set0_cap - 1);
        while (dd->set0_map[k].used) k = (k + 1) & (dd->set0_cap - 1);
        dd->set0_map[k].key = *key;
        dd->set0_map[k].set = s;
        dd->set0_map[k].used = 1;
        dd->set0_n++;
    }
    gpu_vk_unlock(&dd->lock);
    return s;
}

/* ---------------------------------------------------------------------------
 * The empty entries' textures, and the end
 * -------------------------------------------------------------------------*/

/* Without null descriptors: a 1x1 texture cleared to transparent black for
 * each dimension (2D, cube, 2DMS, 2D array) and kind (float, uint). Made at
 * the device's start, so their first layouts are the only ones pending. */
int gpu_vk_desc_init(gpu_vk_device* dd)
{
    if (dd->has_null_desc) return 1;
    VkCommandBuffer cb = gpu_vk_oneshot_begin(dd);
    if (!cb) return 0;
    for (uint32_t dim = 0; dim < 4; dim++)
        for (uint32_t kind = 0; kind < 2; kind++) {
            gpu_texture_desc td;
            memset(&td, 0, sizeof td);
            td.width = td.height = 1;
            td.layers = dim == GPU_DIM_CUBE ? 6 : 1;
            td.mips = 1;
            td.samples = dim == GPU_DIM_2DMS ? 4 : 1;
            td.format = kind ? GPU_FORMAT_R8_UINT : GPU_FORMAT_R8G8B8A8_UNORM;
            td.use = GPU_USE_SAMPLED | GPU_USE_COPY_DST;
            td.state = GPU_STATE_COMMON;
            const gpu_texture t = gpu_vk_texture_create(&dd->base, &td);
            dd->dummy[dim][kind] = t;
            if (!GPU_VALID(t)) continue;
            const gpu_vk_tex* const x = &dd->tex[GPU_HANDLE_INDEX(t.id)];
            VkImageMemoryBarrier2 b;
            VkDependencyInfo di;
            memset(&b, 0, sizeof b);
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = x->image;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
            memset(&di, 0, sizeof di);
            di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            di.imageMemoryBarrierCount = 1;
            di.pImageMemoryBarriers = &b;
            dd->vk.vkCmdPipelineBarrier2(cb, &di);
            const VkClearColorValue zero = {{0.0f, 0.0f, 0.0f, 0.0f}};
            dd->vk.vkCmdClearColorImage(cb, x->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &b.subresourceRange);
            b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            dd->vk.vkCmdPipelineBarrier2(cb, &di);
        }
    const int ok = gpu_vk_oneshot_end(dd, cb);
    gpu_vk_lock(&dd->lock);
    dd->ninit = 0;   /* (done above: only the dummies were pending) */
    gpu_vk_unlock(&dd->lock);
    return ok && GPU_VALID(dd->dummy[0][0]) && GPU_VALID(dd->dummy[0][1]);
}

void gpu_vk_desc_release(gpu_vk_device* dd)
{
    for (uint32_t i = 0; i < dd->nset0_pools; i++) dd->vk.vkDestroyDescriptorPool(dd->device, dd->set0_pools[i], NULL);
    free(dd->set0_pools);
    free(dd->set0_map);
    dd->set0_pools = NULL;
    dd->set0_map = NULL;
    dd->nset0_pools = dd->set0_cap = dd->set0_n = 0;
    for (uint32_t i = 0; i < dd->ntlayout; i++) dd->vk.vkDestroyDescriptorSetLayout(dd->device, dd->tlayout[i].l, NULL);
    dd->ntlayout = 0;
}

#endif /* PS3RECOMP_RSX_HAL && PS3RECOMP_RSX_HAL_VULKAN */
