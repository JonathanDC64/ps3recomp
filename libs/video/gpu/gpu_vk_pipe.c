/*
 * ps3recomp - the portable renderer's GPU layer: the Vulkan backend's pipelines
 *
 * Shaders (SPIR-V modules), layouts (gpu_layout_desc as a pipeline layout:
 * the binding rules in gpu_vk_internal.h), pipelines (dynamic rendering, the
 * states gpu_vk_cmd.h sets dynamic) and the pipeline cache's file.
 *
 * pipeline_create is thread-safe: the start's pipeline workers call it
 * beside the render thread (a VkPipelineCache is internally synchronized).
 *
 * Built with PS3RECOMP_RSX_HAL and PS3RECOMP_RSX_HAL_VULKAN.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(PS3RECOMP_RSX_HAL_VULKAN)

#include "gpu_vk_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>   /* getpid */
#endif

/* ---- step VC1 ---- */
/* A shader's interface, and a pixel shader without some of its outputs.
 * D3D12 drops a vertex attribute the vertex shader does not read and a
 * pixel shader output no target takes (a depth-only pass's colour); Vulkan
 * does too, but its validation reports both. So a pipeline is made with the
 * attributes its vertex shader reads, and with a pixel shader whose outputs
 * past its targets are plain private variables (the stores stay, to
 * nothing). SPIR-V's words: an instruction is its word count and opcode in
 * one word, then its operands. */
enum {
    SPV_OP_ENTRY_POINT = 15, SPV_OP_TYPE_ARRAY = 28, SPV_OP_TYPE_POINTER = 32, SPV_OP_CONSTANT = 43,
    SPV_OP_VARIABLE = 59, SPV_OP_ACCESS_CHAIN = 65, SPV_OP_IN_BOUNDS_ACCESS_CHAIN = 66, SPV_OP_DECORATE = 71,
    SPV_DEC_BUILTIN = 11, SPV_DEC_LOCATION = 30,
    SPV_SC_INPUT = 1, SPV_SC_OUTPUT = 3, SPV_SC_PRIVATE = 6
};

/* What one pass over a module finds: by id, the Location (or -1), whether
 * it is a builtin, a pointer type's storage class, pointee and place (word
 * index), an array's length, a constant's value. 0 when the module is
 * malformed. */
typedef struct spv_ids {
    uint32_t bound;
    int32_t* loc;
    uint8_t* builtin;
    uint32_t* ptr_sc;
    uint32_t* ptr_to;
    uint32_t* ptr_at;
    uint32_t* arr_len;   /* the constant's id, then (after the pass) the length */
    uint32_t* konst;
} spv_ids;

static void spv_ids_free(spv_ids* m)
{
    free(m->loc);
    free(m->builtin);
    free(m->ptr_sc);
    free(m->ptr_to);
    free(m->ptr_at);
    free(m->arr_len);
    free(m->konst);
}

static int spv_ids_read(const uint32_t* w, size_t n, spv_ids* m)
{
    memset(m, 0, sizeof *m);
    if (n < 5 || w[0] != 0x07230203u || w[3] > (1u << 22)) return 0;
    m->bound = w[3];
    m->loc = (int32_t*)malloc(m->bound * sizeof *m->loc);
    m->builtin = (uint8_t*)calloc(m->bound, 1);
    m->ptr_sc = (uint32_t*)calloc(m->bound, sizeof *m->ptr_sc);
    m->ptr_to = (uint32_t*)calloc(m->bound, sizeof *m->ptr_to);
    m->ptr_at = (uint32_t*)calloc(m->bound, sizeof *m->ptr_at);
    m->arr_len = (uint32_t*)calloc(m->bound, sizeof *m->arr_len);
    m->konst = (uint32_t*)calloc(m->bound, sizeof *m->konst);
    if (!m->loc || !m->builtin || !m->ptr_sc || !m->ptr_to || !m->ptr_at || !m->arr_len || !m->konst) {
        spv_ids_free(m);
        return 0;
    }
    for (uint32_t i = 0; i < m->bound; i++) m->loc[i] = -1;
    for (size_t i = 5; i < n;) {
        const uint32_t op = w[i] & 0xFFFFu, wc = w[i] >> 16;
        if (!wc || i + wc > n) {
            spv_ids_free(m);
            return 0;
        }
        const uint32_t* o = &w[i + 1];
        if (op == SPV_OP_DECORATE && wc >= 3 && o[0] < m->bound) {
            if (o[1] == SPV_DEC_LOCATION && wc >= 4) m->loc[o[0]] = (int32_t)o[2];
            if (o[1] == SPV_DEC_BUILTIN) m->builtin[o[0]] = 1;
        } else if (op == SPV_OP_TYPE_POINTER && wc >= 4 && o[0] < m->bound) {
            m->ptr_sc[o[0]] = o[1];
            m->ptr_to[o[0]] = o[2];
            m->ptr_at[o[0]] = (uint32_t)i;
        } else if (op == SPV_OP_TYPE_ARRAY && wc >= 4 && o[0] < m->bound) {
            m->arr_len[o[0]] = o[2];
        } else if (op == SPV_OP_CONSTANT && wc >= 4 && o[1] < m->bound) {
            m->konst[o[1]] = o[2];
        }
        i += wc;
    }
    for (uint32_t i = 0; i < m->bound; i++)
        if (m->arr_len[i]) m->arr_len[i] = m->arr_len[i] < m->bound ? m->konst[m->arr_len[i]] : 0u;
    return 1;
}

/* The Locations a variable of pointer type ptr at Location loc takes: one,
 * or an array's length. */
static uint32_t spv_loc_bits(const spv_ids* m, uint32_t ptr, int32_t loc)
{
    if (loc < 0 || loc >= 32 || ptr >= m->bound) return 0;
    const uint32_t t = m->ptr_to[ptr];
    uint32_t len = t < m->bound && m->arr_len[t] ? m->arr_len[t] : 1u;
    if (len > 32u - (uint32_t)loc) len = 32u - (uint32_t)loc;
    return (uint32_t)(((1ull << len) - 1u) << loc);
}

/* The Locations of the module's Input and Output variables, builtins left
 * out. When the module does not read: every input, and no outputs (so
 * every attribute is given and no copy is made). */
static void spv_interface(const uint32_t* w, size_t n, uint32_t* in_mask, uint32_t* out_mask)
{
    spv_ids m;
    *in_mask = *out_mask = 0;
    if (!spv_ids_read(w, n, &m)) {
        *in_mask = ~0u;
        return;
    }
    for (size_t i = 5; i < n;) {
        const uint32_t op = w[i] & 0xFFFFu, wc = w[i] >> 16;
        const uint32_t* o = &w[i + 1];
        if (op == SPV_OP_VARIABLE && wc >= 4 && o[1] < m.bound && !m.builtin[o[1]]) {
            if (o[2] == SPV_SC_INPUT) *in_mask |= spv_loc_bits(&m, o[0], m.loc[o[1]]);
            if (o[2] == SPV_SC_OUTPUT) *out_mask |= spv_loc_bits(&m, o[0], m.loc[o[1]]);
        }
        i += wc;
    }
    spv_ids_free(&m);
}

/* A copy of a module (malloc'd, *out_n words) whose Output variables at
 * Locations in `drop` are Private: out of the entry point's interface, their
 * decorations gone, their pointer types (and those of access chains into
 * them) Private ones. A Private pointer type to the same type is used where
 * the module declares one before the Output one; one it declares later is
 * moved up to just after the Output one (an id is declared before its use);
 * else one is made there. NULL on failure. */
static uint32_t* spv_drop_outputs(const uint32_t* w, size_t n, uint32_t drop, size_t* out_n)
{
    spv_ids m;
    if (!spv_ids_read(w, n, &m)) return NULL;
    uint8_t* gone = (uint8_t*)calloc(m.bound, 1);        /* the variables, and chains into them */
    uint32_t* twin = (uint32_t*)calloc(m.bound, sizeof(uint32_t));   /* Output pointer type -> Private one */
    uint8_t* made = (uint8_t*)calloc(m.bound, 1);        /* the twin is emitted just after the type */
    uint8_t* moved = (uint8_t*)calloc(m.bound, 1);       /* a Private type emitted earlier, not where it was */
    uint32_t* out = (uint32_t*)malloc((n + 4u * 64u) * sizeof(uint32_t));
    uint32_t bound = m.bound, nnew = 0;
    size_t k = 5;
    if (!gone || !twin || !made || !moved || !out) goto fail;
    for (size_t i = 5; i < n;) {   /* the variables, the chains, the pointer types they need */
        const uint32_t op = w[i] & 0xFFFFu, wc = w[i] >> 16;
        const uint32_t* o = &w[i + 1];
        uint32_t ptr = 0;
        if (op == SPV_OP_VARIABLE && wc >= 4 && o[1] < m.bound && o[2] == SPV_SC_OUTPUT && !m.builtin[o[1]]) {
            const uint32_t bits = spv_loc_bits(&m, o[0], m.loc[o[1]]);
            if (bits && !(bits & ~drop)) {
                gone[o[1]] = 1;
                ptr = o[0];
            }
        } else if ((op == SPV_OP_ACCESS_CHAIN || op == SPV_OP_IN_BOUNDS_ACCESS_CHAIN) && wc >= 4 &&
                   o[1] < m.bound && o[2] < m.bound && gone[o[2]]) {
            gone[o[1]] = 1;
            ptr = o[0];
        }
        if (ptr && ptr < m.bound && !twin[ptr]) {
            for (uint32_t t = 1; t < m.bound && !twin[ptr]; t++) {
                if (m.ptr_sc[t] != SPV_SC_PRIVATE || m.ptr_to[t] != m.ptr_to[ptr] || moved[t]) continue;
                twin[ptr] = t;
                if (m.ptr_at[t] > m.ptr_at[ptr]) {
                    moved[t] = 1;
                    made[ptr] = 1;
                }
            }
            if (!twin[ptr]) {
                if (++nnew > 64) goto fail;
                twin[ptr] = bound++;
                made[ptr] = 1;
            }
        }
        i += wc;
    }
    memcpy(out, w, 5 * sizeof(uint32_t));
    out[3] = bound;
    for (size_t i = 5; i < n;) {
        const uint32_t op = w[i] & 0xFFFFu, wc = w[i] >> 16;
        const uint32_t* o = &w[i + 1];
        if (op == SPV_OP_ENTRY_POINT && wc >= 3) {   /* execution model, id, name (a string), interface */
            size_t s = 3;
            while (s < wc && (w[i + s] >> 24) != 0) s++;   /* the string ends at a word with a zero top byte */
            s++;
            const size_t at = k;
            for (size_t j = i; j < i + s && j < i + wc; j++) out[k++] = w[j];
            for (size_t j = i + s; j < i + wc; j++)
                if (!(w[j] < m.bound && gone[w[j]])) out[k++] = w[j];
            out[at] = ((uint32_t)(k - at) << 16) | op;
        } else if (op == SPV_OP_DECORATE && wc >= 3 && o[0] < m.bound && gone[o[0]]) {
            /* dropped: a Private variable takes no Location */
        } else if (op == SPV_OP_TYPE_POINTER && wc >= 4 && o[0] < m.bound && moved[o[0]]) {
            /* emitted earlier, after the Output type it stands in for */
        } else if ((op == SPV_OP_VARIABLE || op == SPV_OP_ACCESS_CHAIN || op == SPV_OP_IN_BOUNDS_ACCESS_CHAIN) &&
                   wc >= 4 && o[1] < m.bound && gone[o[1]]) {
            for (size_t j = i; j < i + wc; j++) out[k++] = w[j];
            out[k - wc + 1] = twin[o[0]];
            if (op == SPV_OP_VARIABLE) out[k - wc + 3] = SPV_SC_PRIVATE;
        } else {
            for (size_t j = i; j < i + wc; j++) out[k++] = w[j];
            if (op == SPV_OP_TYPE_POINTER && wc >= 4 && o[0] < m.bound && made[o[0]]) {
                out[k++] = (4u << 16) | SPV_OP_TYPE_POINTER;
                out[k++] = twin[o[0]];
                out[k++] = SPV_SC_PRIVATE;
                out[k++] = o[2];
            }
        }
        i += wc;
    }
    free(gone);
    free(twin);
    free(made);
    free(moved);
    spv_ids_free(&m);
    *out_n = k;
    return out;
fail:
    free(gone);
    free(twin);
    free(made);
    free(moved);
    free(out);
    spv_ids_free(&m);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Shaders
 * -------------------------------------------------------------------------*/

/* SPIR-V only (GPU_CODE_SPIRV); the module copies the code. Thread-safe. */
gpu_shader gpu_vk_shader_create(gpu_device* d, const gpu_shader_desc* desc)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_shader r = {0};
    if (desc->code != GPU_CODE_SPIRV || !desc->bytes || !desc->size || (desc->size & 3u) || desc->stage > GPU_STAGE_PS)
        return r;
    if (desc->stage == GPU_STAGE_GS && !dd->has_gs) return r;
    VkShaderModuleCreateInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    si.codeSize = (size_t)desc->size;
    si.pCode = (const uint32_t*)desc->bytes;
    VkShaderModule m = VK_NULL_HANDLE;
    if (dd->vk.vkCreateShaderModule(dd->device, &si, NULL, &m) != VK_SUCCESS) return r;
    /* Step VC1: its interface, and a pixel shader's code for the copies
     * without some outputs. */
    uint32_t in_mask = 0, out_mask = 0;
    spv_interface((const uint32_t*)desc->bytes, (size_t)(desc->size / 4u), &in_mask, &out_mask);
    uint32_t* words = NULL;
    if (desc->stage == GPU_STAGE_PS && out_mask) {
        words = (uint32_t*)malloc((size_t)desc->size);
        if (words) memcpy(words, desc->bytes, (size_t)desc->size);
    }
    gpu_vk_lock(&dd->lock);
    r.id = gpu_vk_slot_alloc(&dd->shader_slots);
    if (r.id) {
        gpu_vk_shader* const s = &dd->shader[GPU_HANDLE_INDEX(r.id)];
        s->module = m;
        s->stage = desc->stage;
        s->hash = gpu_vk_fnv(GPU_VK_FNV_BASIS, desc->bytes, (size_t)desc->size);
        snprintf(s->entry, sizeof s->entry, "%s", desc->entry ? desc->entry : "main");
        s->in_mask = in_mask;
        s->out_mask = out_mask;
        s->words = words;
        s->nwords = words ? (uint32_t)(desc->size / 4u) : 0u;
    }
    gpu_vk_unlock(&dd->lock);
    if (!r.id) {
        dd->vk.vkDestroyShaderModule(dd->device, m, NULL);
        free(words);
    }
    return r;
}

void gpu_vk_shader_destroy(gpu_device* d, gpu_shader s)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    VkShaderModule m = VK_NULL_HANDLE;
    uint32_t* words = NULL;
    gpu_vk_lock(&dd->lock);
    if (gpu_vk_slot_valid(&dd->shader_slots, s.id)) {
        gpu_vk_shader* const sh = &dd->shader[GPU_HANDLE_INDEX(s.id)];
        m = sh->module;
        words = sh->words;   /* (step VC1) */
        memset(sh, 0, sizeof *sh);
        gpu_vk_slot_free(&dd->shader_slots, s.id);
    }
    gpu_vk_unlock(&dd->lock);
    if (m) dd->vk.vkDestroyShaderModule(dd->device, m, NULL);
    free(words);
}

/* ---------------------------------------------------------------------------
 * Layouts
 * -------------------------------------------------------------------------*/

static VkShaderStageFlags stages_of(const gpu_vk_device* dd, uint32_t vis)
{
    const VkShaderStageFlags all = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                   (dd->has_gs ? VK_SHADER_STAGE_GEOMETRY_BIT : 0u);
    switch (vis) {
    case GPU_VIS_VERTEX:   return VK_SHADER_STAGE_VERTEX_BIT;
    case GPU_VIS_PIXEL:    return VK_SHADER_STAGE_FRAGMENT_BIT;
    case GPU_VIS_GEOMETRY: return dd->has_gs ? VK_SHADER_STAGE_GEOMETRY_BIT : all;
    default:               return all;
    }
}

/* A static sampler's border (gpu.h: transparent black, opaque black or
 * opaque white). */
static VkBorderColor border_of(const gpu_sampler_desc* s)
{
    if (s->border[3] == 0.0f) return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    return s->border[0] == 0.0f ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
}

/* gpu_layout_desc as a pipeline layout (gpu_vk_internal.h's rules): set 0
 * the CB and RAW slots as dynamic buffers and the static samplers as
 * immutable ones, a set per table slot, the CONST32 slots one push
 * constant block. Table slots start at register 0. */
gpu_layout gpu_vk_layout_create(gpu_device* d, const gpu_layout_desc* desc)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_layout r = {0};
    gpu_vk_layout l;
    VkDescriptorSetLayoutBinding b[GPU_MAX_SLOTS + GPU_MAX_STATIC_SAMPLERS];
    VkDescriptorSetLayout sl[GPU_VK_MAX_SETS];
    uint32_t nb = 0;
    if (desc->nslots > GPU_MAX_SLOTS || desc->nstatic > GPU_MAX_STATIC_SAMPLERS) return r;
    memset(&l, 0, sizeof l);
    memset(b, 0, sizeof b);
    l.nsets = 1;
    for (uint32_t i = 0; i < desc->nslots; i++) {
        const gpu_layout_slot* const s = &desc->slot[i];
        l.slot[i].kind = s->kind;
        switch (s->kind) {
        case GPU_SLOT_CB:
        case GPU_SLOT_RAW:
            /* b0-b15 at 0-15, t0-t31 at 16-47 (the static samplers' from 48). */
            if (s->reg >= (s->kind == GPU_SLOT_CB ? 16u : 32u)) {
                printf("[VK] layout: slot %u's register %u is past the bindings\n", i, s->reg);
                return r;
            }
            b[nb].binding = (s->kind == GPU_SLOT_CB ? GPU_VK_BIND_CB : GPU_VK_BIND_T) + s->reg;
            b[nb].descriptorType = s->kind == GPU_SLOT_CB ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                                          : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            b[nb].descriptorCount = 1;
            b[nb].stageFlags = stages_of(dd, s->visibility);
            nb++;
            break;
        case GPU_SLOT_CONST32:
            l.slot[i].push_off = (uint16_t)l.push_size;
            l.slot[i].push_size = (uint16_t)(s->count * 4u);
            l.push_size += s->count * 4u;
            l.push_stages |= stages_of(dd, s->visibility);
            break;
        case GPU_SLOT_TABLE_TEX:
        case GPU_SLOT_TABLE_SAMPLER:
            if (s->reg != 0) {
                printf("[VK] layout: a table slot must start at register 0 (slot %u)\n", i);
                return r;
            }
            l.slot[i].set = (uint8_t)l.nsets++;
            break;
        default:
            return r;
        }
    }
    if (l.push_size > dd->props.limits.maxPushConstantsSize) return r;
    /* Dynamic offsets in binding order (vkCmdBindDescriptorSets takes them so). */
    for (uint32_t bind = 0; bind < GPU_VK_BIND_S; bind++)
        for (uint32_t i = 0; i < desc->nslots; i++) {
            const gpu_layout_slot* const s = &desc->slot[i];
            if ((s->kind == GPU_SLOT_CB && GPU_VK_BIND_CB + s->reg == bind) ||
                (s->kind == GPU_SLOT_RAW && GPU_VK_BIND_T + s->reg == bind)) {
                l.slot[i].dyn = (uint8_t)l.ndyn;
                l.dyn_raw[l.ndyn] = s->kind == GPU_SLOT_RAW;
                l.dyn_binding[l.ndyn] = bind;
                l.ndyn++;
            }
        }
    for (uint32_t k = 0; k < desc->nstatic; k++) {
        const gpu_static_sampler* const ss = &desc->statics[k];
        if (ss->reg >= 16) goto fail;
        l.statics[k] = gpu_vk_sampler_make(dd, &ss->desc, border_of(&ss->desc));
        if (!l.statics[k]) goto fail;
        l.nstatics++;
        b[nb].binding = GPU_VK_BIND_S + ss->reg;
        b[nb].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        b[nb].descriptorCount = 1;
        b[nb].stageFlags = stages_of(dd, ss->visibility);
        b[nb].pImmutableSamplers = &l.statics[k];
        nb++;
    }
    {
        VkDescriptorSetLayoutCreateInfo li;
        memset(&li, 0, sizeof li);
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = nb;
        li.pBindings = b;
        if (dd->vk.vkCreateDescriptorSetLayout(dd->device, &li, NULL, &l.set0) != VK_SUCCESS) goto fail;
    }
    sl[0] = l.set0;
    gpu_vk_lock(&dd->lock);
    for (uint32_t i = 0; i < desc->nslots; i++)
        if (desc->slot[i].kind == GPU_SLOT_TABLE_TEX || desc->slot[i].kind == GPU_SLOT_TABLE_SAMPLER)
            sl[l.slot[i].set] = gpu_vk_table_layout(dd, desc->slot[i].kind == GPU_SLOT_TABLE_TEX ? GPU_TABLE_TEX
                                                                                                 : GPU_TABLE_SAMPLER,
                                                    desc->slot[i].count);
    /* Static samplers alone: one set, made here and bound with the layout. */
    if (nb && !l.ndyn) l.set0_fixed = gpu_vk_set0_alloc(dd, l.set0);
    gpu_vk_unlock(&dd->lock);
    for (uint32_t s = 1; s < l.nsets; s++)
        if (!sl[s]) goto fail;
    if (nb && !l.ndyn && !l.set0_fixed) goto fail;
    l.first = nb ? 0u : 1u;
    l.sets_mask = ((1u << l.nsets) - 1u) & ~(l.first ? 1u : 0u);
    {
        VkPushConstantRange pr;
        VkPipelineLayoutCreateInfo pi;
        pr.stageFlags = l.push_stages;
        pr.offset = 0;
        pr.size = l.push_size;
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pi.setLayoutCount = l.nsets;
        pi.pSetLayouts = sl;
        pi.pushConstantRangeCount = l.push_size ? 1u : 0u;
        pi.pPushConstantRanges = &pr;
        if (dd->vk.vkCreatePipelineLayout(dd->device, &pi, NULL, &l.pl) != VK_SUCCESS) goto fail;
    }
    gpu_vk_lock(&dd->lock);
    r.id = gpu_vk_slot_alloc(&dd->layout_slots);
    if (r.id) {
        l.index = GPU_HANDLE_INDEX(r.id) + 1u;
        dd->layout[GPU_HANDLE_INDEX(r.id)] = l;
    }
    gpu_vk_unlock(&dd->lock);
    if (r.id) return r;
fail:
    if (l.pl) dd->vk.vkDestroyPipelineLayout(dd->device, l.pl, NULL);
    if (l.set0) dd->vk.vkDestroyDescriptorSetLayout(dd->device, l.set0, NULL);
    for (uint32_t k = 0; k < l.nstatics; k++) dd->vk.vkDestroySampler(dd->device, l.statics[k], NULL);
    return r;
}

/* ---------------------------------------------------------------------------
 * Pipelines
 * -------------------------------------------------------------------------*/

static VkBlendFactor blend_of(uint32_t b)
{
    switch (b) {
    case GPU_BLEND_ZERO:             return VK_BLEND_FACTOR_ZERO;
    case GPU_BLEND_ONE:              return VK_BLEND_FACTOR_ONE;
    case GPU_BLEND_SRC_COLOR:        return VK_BLEND_FACTOR_SRC_COLOR;
    case GPU_BLEND_INV_SRC_COLOR:    return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case GPU_BLEND_SRC_ALPHA:        return VK_BLEND_FACTOR_SRC_ALPHA;
    case GPU_BLEND_INV_SRC_ALPHA:    return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GPU_BLEND_DEST_ALPHA:       return VK_BLEND_FACTOR_DST_ALPHA;
    case GPU_BLEND_INV_DEST_ALPHA:   return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case GPU_BLEND_DEST_COLOR:       return VK_BLEND_FACTOR_DST_COLOR;
    case GPU_BLEND_INV_DEST_COLOR:   return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case GPU_BLEND_SRC_ALPHA_SAT:    return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case GPU_BLEND_BLEND_FACTOR:     return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case GPU_BLEND_INV_BLEND_FACTOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case GPU_BLEND_SRC1_COLOR:       return VK_BLEND_FACTOR_SRC1_COLOR;
    case GPU_BLEND_INV_SRC1_COLOR:   return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case GPU_BLEND_SRC1_ALPHA:       return VK_BLEND_FACTOR_SRC1_ALPHA;
    case GPU_BLEND_INV_SRC1_ALPHA:   return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    default:                         return VK_BLEND_FACTOR_ONE;
    }
}

/* GPU_CMP_*, GPU_STENCIL_* and GPU_BLEND_OP_* are Vulkan's numbers plus one. */
static uint32_t less1(uint32_t v, uint32_t none)
{
    return v ? v - 1u : none;
}

static void stencil_of(const gpu_stencil_face* f, const gpu_pipeline_desc* desc, VkStencilOpState* s)
{
    s->failOp = (VkStencilOp)less1(f->fail, VK_STENCIL_OP_KEEP);
    s->passOp = (VkStencilOp)less1(f->pass, VK_STENCIL_OP_KEEP);
    s->depthFailOp = (VkStencilOp)less1(f->depth_fail, VK_STENCIL_OP_KEEP);
    s->compareOp = (VkCompareOp)less1(f->func, VK_COMPARE_OP_ALWAYS);
    s->compareMask = desc->stencil_read_mask;
    s->writeMask = desc->stencil_write_mask;
    s->reference = 0;
}

/* A pipeline for dynamic rendering into four colour slots (the pass always
 * has four; slots past nrt UNDEFINED and written by nothing) and the depth
 * format's aspects. Dynamic: viewport, scissor, topology (within the type),
 * primitive restart, stencil reference, blend constants, and depth writes
 * when the pipeline writes depth. depth_clip 0 is depth clamping (without
 * GPU_CAP_DEPTH_CLAMP, clipping). The vertex input's stride is the
 * description's (set_vb's is not used). From the cache: the creation
 * feedback's cache hit; with no hit (or no feedback) the driver has put it
 * in the cache. GPU_PIPELINE_STORE_ONLY: made with the cache, so the cache
 * has it, then destroyed at once (nothing has recorded it) and not kept. */
gpu_pipeline gpu_vk_pipeline_create(gpu_device* d, const gpu_pipeline_desc* desc, uint32_t flags, uint32_t* how)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_pipeline r = {0};
    if (how) *how = GPU_PIPELINE_FAILED;
    VkPipelineShaderStageCreateInfo st[3];
    uint32_t nst = 0;
    const gpu_shader sh[3] = {desc->vs, desc->gs, desc->ps};
    const VkShaderStageFlagBits stage[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                                            VK_SHADER_STAGE_FRAGMENT_BIT};
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipelineCache cache;
    uint32_t vs_in = ~0u, ps_out = 0, ps_st = 0, ps_nwords = 0;   /* (step VC1) */
    const uint32_t* ps_words = NULL;
    memset(st, 0, sizeof st);
    gpu_vk_lock(&dd->lock);
    if (gpu_vk_slot_valid(&dd->layout_slots, desc->layout.id)) pl = dd->layout[GPU_HANDLE_INDEX(desc->layout.id)].pl;
    for (uint32_t k = 0; k < 3; k++) {
        if (!GPU_VALID(sh[k])) continue;
        if (!gpu_vk_slot_valid(&dd->shader_slots, sh[k].id)) {
            pl = VK_NULL_HANDLE;
            break;
        }
        const gpu_vk_shader* const s = &dd->shader[GPU_HANDLE_INDEX(sh[k].id)];
        st[nst].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[nst].stage = stage[k];
        st[nst].module = s->module;
        st[nst].pName = s->entry;   /* (kept: shader_destroy waits for no pipeline_create) */
        if (k == 0) vs_in = s->in_mask;
        if (k == 2) {
            ps_out = s->out_mask;
            ps_words = s->words;   /* (kept, as the entry is) */
            ps_nwords = s->nwords;
            ps_st = nst;
        }
        nst++;
    }
    gpu_vk_unlock(&dd->lock);
    if (!pl || !GPU_VALID(desc->vs) || (GPU_VALID(desc->gs) && !dd->has_gs) || desc->nrt > 4) return r;

    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va[GPU_MAX_VERTEX_ATTRS];
    VkPipelineVertexInputStateCreateInfo vi;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (desc->vertex.count) {
        if (desc->vertex.count > GPU_MAX_VERTEX_ATTRS) return r;
        vb.binding = 0;
        vb.stride = desc->vertex.stride;
        vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        uint32_t na = 0;
        for (uint32_t k = 0; k < desc->vertex.count; k++) {
            const gpu_vk_format* const f = gpu_vk_format_of(desc->vertex.attr[k].format, 0);
            if (!f) return r;
            if (k < 32 && !((vs_in >> k) & 1u)) continue;   /* (step VC1: the shader does not read it) */
            va[na].location = k;
            va[na].binding = 0;
            va[na].format = f->vk;
            va[na].offset = desc->vertex.attr[k].offset;
            na++;
        }
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = na;
        vi.pVertexAttributeDescriptions = va;
    }
    VkPipelineInputAssemblyStateCreateInfo ia;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = desc->topology_type == GPU_TOPOLOGY_TYPE_POINT  ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                  : desc->topology_type == GPU_TOPOLOGY_TYPE_LINE ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                                  : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.depthClampEnable = !desc->depth_clip && (dd->base.caps.bits & GPU_CAP_DEPTH_CLAMP) ? VK_TRUE : VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = desc->cull == GPU_CULL_FRONT  ? VK_CULL_MODE_FRONT_BIT
                  : desc->cull == GPU_CULL_BACK ? VK_CULL_MODE_BACK_BIT
                                                : VK_CULL_MODE_NONE;
    rs.frontFace = desc->front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms;
    const VkSampleMask mask = desc->sample_mask ? desc->sample_mask : 0xFFFFFFFFu;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(desc->samples > 1 ? desc->samples : 1u);
    ms.pSampleMask = &mask;
    VkPipelineDepthStencilStateCreateInfo ds;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = desc->depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = desc->depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = (VkCompareOp)less1(desc->depth_func, VK_COMPARE_OP_ALWAYS);
    ds.stencilTestEnable = desc->stencil_test ? VK_TRUE : VK_FALSE;
    stencil_of(&desc->stencil_front, desc, &ds.front);
    stencil_of(&desc->stencil_back, desc, &ds.back);
    ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState ba[4];
    VkPipelineColorBlendStateCreateInfo cb;
    memset(ba, 0, sizeof ba);
    for (uint32_t k = 0; k < 4; k++) {
        const gpu_blend_state* const b = &desc->blend;
        if (k >= desc->nrt && dd->has_independent_blend) continue;   /* (unused: nothing written) */
        ba[k].blendEnable = b->enable ? VK_TRUE : VK_FALSE;
        ba[k].srcColorBlendFactor = blend_of(b->src);
        ba[k].dstColorBlendFactor = blend_of(b->dst);
        ba[k].colorBlendOp = (VkBlendOp)less1(b->op, VK_BLEND_OP_ADD);
        ba[k].srcAlphaBlendFactor = blend_of(b->src_alpha);
        ba[k].dstAlphaBlendFactor = blend_of(b->dst_alpha);
        ba[k].alphaBlendOp = (VkBlendOp)less1(b->op_alpha, VK_BLEND_OP_ADD);
        ba[k].colorWriteMask = b->write_mask & 15u;
    }
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 4;
    cb.pAttachments = ba;
    cb.blendConstants[0] = cb.blendConstants[1] = cb.blendConstants[2] = cb.blendConstants[3] = 1.0f;
    VkDynamicState dyn[7] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
                             VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                             VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE};
    VkPipelineDynamicStateCreateInfo dy;
    memset(&dy, 0, sizeof dy);
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = desc->depth_write ? 7u : 6u;
    dy.pDynamicStates = dyn;
    VkFormat cf[4];
    for (uint32_t k = 0; k < 4; k++) {
        const gpu_vk_format* const f = k < desc->nrt ? gpu_vk_format_of(desc->rt_format[k], GPU_USE_RT) : NULL;
        if (k < desc->nrt && !f) return r;
        cf[k] = f ? f->vk : VK_FORMAT_UNDEFINED;
    }
    VkPipelineRenderingCreateInfo ri;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ri.colorAttachmentCount = 4;
    ri.pColorAttachmentFormats = cf;
    if (desc->ds_format) {
        const gpu_vk_format* const f = gpu_vk_format_dev(dd, desc->ds_format, GPU_USE_DS);
        if (!f) return r;
        if (f->aspect & GPU_VK_ASPECT_DEPTH) ri.depthAttachmentFormat = f->vk;
        if (f->aspect & GPU_VK_ASPECT_STENCIL) ri.stencilAttachmentFormat = f->vk;
    }
    VkPipelineCreationFeedback fb;
    VkPipelineCreationFeedbackCreateInfo fi;
    memset(&fb, 0, sizeof fb);
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO;
    fi.pPipelineCreationFeedback = &fb;
    ri.pNext = &fi;
    VkGraphicsPipelineCreateInfo gi;
    memset(&gi, 0, sizeof gi);
    gi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gi.pNext = &ri;
    gi.stageCount = nst;
    gi.pStages = st;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = pl;
    /* Step VC1: the pixel shader's outputs no target takes, made private in
     * a copy of its module for this pipeline only. */
    uint32_t rt_mask = 0;
    for (uint32_t k = 0; k < 4; k++)
        if (cf[k] != VK_FORMAT_UNDEFINED) rt_mask |= 1u << k;
    VkShaderModule ps_tmp = VK_NULL_HANDLE;
    if ((ps_out & ~rt_mask) && ps_words) {
        size_t n = 0;
        uint32_t* const w = spv_drop_outputs(ps_words, ps_nwords, ps_out & ~rt_mask, &n);
        if (w) {
            VkShaderModuleCreateInfo si;
            memset(&si, 0, sizeof si);
            si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            si.codeSize = n * sizeof(uint32_t);
            si.pCode = w;
            if (dd->vk.vkCreateShaderModule(dd->device, &si, NULL, &ps_tmp) == VK_SUCCESS) st[ps_st].module = ps_tmp;
            else ps_tmp = VK_NULL_HANDLE;
            free(w);
        }
    }
    gpu_vk_lock(&dd->pso_lock);
    cache = (dd->base.caps.bits & GPU_CAP_PIPELINE_CACHE) ? dd->pcache : VK_NULL_HANDLE;
    gpu_vk_unlock(&dd->pso_lock);
    VkPipeline p = VK_NULL_HANDLE;
    const VkResult cr = dd->vk.vkCreateGraphicsPipelines(dd->device, cache, 1, &gi, NULL, &p);
    if (ps_tmp) dd->vk.vkDestroyShaderModule(dd->device, ps_tmp, NULL);
    if (cr != VK_SUCCESS) return r;
    const uint32_t made = !cache ? GPU_PIPELINE_COMPILED
                        : (fb.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) &&
                          (fb.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT)
                        ? GPU_PIPELINE_LOADED : GPU_PIPELINE_STORED;
    gpu_vk_lock(&dd->pso_lock);   /* (step VC2: PS3_PSO_LOG's figures, gpu_vk_pipe_release) */
    dd->pso_made++;
    if (made == GPU_PIPELINE_LOADED) dd->pso_hits++;
    if (fb.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) dd->pso_ns += fb.duration;
    gpu_vk_unlock(&dd->pso_lock);
    if (flags & GPU_PIPELINE_STORE_ONLY) {
        dd->vk.vkDestroyPipeline(dd->device, p, NULL);
        if (how) *how = made;
        return r;
    }
    uint32_t idx = UINT32_MAX;
    gpu_vk_lock(&dd->pso_lock);
    if (dd->npso < GPU_VK_MAX_PIPELINES) {
        idx = dd->npso++;
        dd->pso[idx].p = p;
        dd->pso[idx].dyn_dw = desc->depth_write ? 1u : 0u;
    }
    gpu_vk_unlock(&dd->pso_lock);
    if (idx == UINT32_MAX) {
        dd->vk.vkDestroyPipeline(dd->device, p, NULL);
        return r;
    }
    if (how) *how = made;
    r.v = (uint64_t)idx + 1u;
    return r;
}

/* ---------------------------------------------------------------------------
 * The pipeline cache's file
 * -------------------------------------------------------------------------*/

/* A header names the driver the data is for, and says how long the data is
 * and what it sums to (step VC2: with a version and its own length, so a
 * later layout is told apart). Vulkan's own header, at the data's start, is
 * checked too (pcache_vk_ok): a file is checked whole before the driver sees
 * any of it. */
#define PCACHE_MAGIC   "PS3VKPC\0"
#define PCACHE_VERSION 2u
typedef struct pcache_head {
    char     magic[8];        /* PCACHE_MAGIC */
    uint32_t version;         /* PCACHE_VERSION */
    uint32_t head_size;       /* sizeof(pcache_head) */
    uint32_t vendor, device, driver;
    uint8_t  uuid[VK_UUID_SIZE];
    uint32_t pad;             /* (zero) */
    uint64_t size, fnv;       /* the driver's data after it: its bytes, their FNV-1a 64 */
} pcache_head;

/* Step VC2: the data starts with Vulkan's header for this device
 * (VkPipelineCacheHeaderVersionOne: its length, version 1, vendor, device,
 * the pipeline cache's UUID). */
static int pcache_vk_ok(const gpu_vk_device* dd, const uint8_t* data, uint64_t size)
{
    VkPipelineCacheHeaderVersionOne vh;
    if (size < sizeof vh) return 0;
    memcpy(&vh, data, sizeof vh);
    return vh.headerSize >= sizeof vh && vh.headerSize <= size &&
           vh.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && vh.vendorID == dd->props.vendorID &&
           vh.deviceID == dd->props.deviceID && !memcmp(vh.pipelineCacheUUID, dd->props.pipelineCacheUUID, VK_UUID_SIZE);
}

/* GPU_PIPELINE_CACHE_LOADED when the file was this driver's and now seeds
 * the cache, else _NEW: the device's own cache, made with it (empty before
 * the first pipeline), stays, and the next save replaces the file. A file
 * is this driver's when every check holds: our header (magic, version,
 * length, the driver, the data's length to the file's end, its sum) and
 * Vulkan's. _NONE with the cap masked: pipelines are made without the
 * cache. The old cache is kept until the device goes (a pipeline worker may
 * be using it). */
int gpu_vk_pipeline_cache_load(gpu_device* d, const char* path)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    if (!(d->caps.bits & GPU_CAP_PIPELINE_CACHE)) return GPU_PIPELINE_CACHE_NONE;
    FILE* f = path ? fopen(path, "rb") : NULL;
    pcache_head h;
    if (!f) return GPU_PIPELINE_CACHE_NEW;
    void* data = NULL;
    int ok = fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, PCACHE_MAGIC, 8) && h.version == PCACHE_VERSION &&
             h.head_size == sizeof h && h.vendor == dd->props.vendorID && h.device == dd->props.deviceID &&
             h.driver == dd->props.driverVersion && !memcmp(h.uuid, dd->props.pipelineCacheUUID, VK_UUID_SIZE) &&
             h.size && h.size < (1ull << 31);
    if (ok) {
        data = malloc((size_t)h.size + 1);
        ok = data && fread(data, (size_t)h.size, 1, f) == 1 && fgetc(f) == EOF &&   /* (no more, no less) */
             gpu_vk_fnv(GPU_VK_FNV_BASIS, data, (size_t)h.size) == h.fnv &&
             pcache_vk_ok(dd, (const uint8_t*)data, h.size);
    }
    fclose(f);
    VkPipelineCache nc = VK_NULL_HANDLE;
    if (ok) {
        VkPipelineCacheCreateInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        ci.initialDataSize = (size_t)h.size;
        ci.pInitialData = data;
        ok = dd->vk.vkCreatePipelineCache(dd->device, &ci, NULL, &nc) == VK_SUCCESS;
    }
    free(data);
    if (!ok) return GPU_PIPELINE_CACHE_NEW;
    gpu_vk_lock(&dd->pso_lock);
    if (dd->nold_caches < 8) {
        dd->old_caches[dd->nold_caches++] = dd->pcache;
        dd->pcache = nc;
        nc = VK_NULL_HANDLE;
    }
    gpu_vk_unlock(&dd->pso_lock);
    if (nc) {   /* (eight loads already: keep the cache there is) */
        dd->vk.vkDestroyPipelineCache(dd->device, nc, NULL);
        return GPU_PIPELINE_CACHE_NEW;
    }
    return GPU_PIPELINE_CACHE_LOADED;
}

/* Written to a temporary file beside it, then moved over path, so a crash
 * leaves the old file. */
int gpu_vk_pipeline_cache_save(gpu_device* d, const char* path)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    if (!path || !(d->caps.bits & GPU_CAP_PIPELINE_CACHE)) return 0;
    VkPipelineCache cache;
    gpu_vk_lock(&dd->pso_lock);
    cache = dd->pcache;
    gpu_vk_unlock(&dd->pso_lock);
    size_t size = 0;
    if (dd->vk.vkGetPipelineCacheData(dd->device, cache, &size, NULL) != VK_SUCCESS || !size) return 0;
    void* data = malloc(size);
    if (!data) return 0;
    VkResult vr = dd->vk.vkGetPipelineCacheData(dd->device, cache, &size, data);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE) {
        free(data);
        return 0;
    }
    pcache_head h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, PCACHE_MAGIC, 8);
    h.version = PCACHE_VERSION;
    h.head_size = sizeof h;
    h.vendor = dd->props.vendorID;
    h.device = dd->props.deviceID;
    h.driver = dd->props.driverVersion;
    memcpy(h.uuid, dd->props.pipelineCacheUUID, VK_UUID_SIZE);
    h.size = size;
    h.fnv = gpu_vk_fnv(GPU_VK_FNV_BASIS, data, size);
    /* (step P3S: named by the process and thread, so two saving at once,
     * two games or the save thread and the shutdown, never share one) */
    char tmp[1024];
#ifdef _WIN32
    snprintf(tmp, sizeof tmp, "%s.%lu.%lu.tmp", path, (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetCurrentThreadId());
#else
    snprintf(tmp, sizeof tmp, "%s.%ld.%lu.tmp", path, (long)getpid(), (unsigned long)pthread_self());
#endif
    FILE* f = fopen(tmp, "wb");
    int ok = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(data, size, 1, f) == 1;
    if (f && fclose(f)) ok = 0;
    free(data);
    if (!ok) {
        remove(tmp);
        return 0;
    }
#ifdef _WIN32
    ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    ok = rename(tmp, path) == 0;
#endif
    if (!ok) remove(tmp);
    return ok;
}

/* With the device: every pipeline, cache and layout. */
void gpu_vk_pipe_release(gpu_vk_device* dd)
{
    if (dd->pso_made && getenv("PS3_PSO_LOG"))   /* (step VC2: what the pipeline cache's file saves) */
        printf("[VK] pipelines made on this device: %u, %u of them from the pipeline cache; %.1f ms in the driver\n",
               dd->pso_made, dd->pso_hits, (double)dd->pso_ns / 1e6);
    if (dd->pso)
        for (uint32_t i = 0; i < dd->npso; i++) dd->vk.vkDestroyPipeline(dd->device, dd->pso[i].p, NULL);
    dd->npso = 0;
    if (dd->pcache) dd->vk.vkDestroyPipelineCache(dd->device, dd->pcache, NULL);
    for (uint32_t i = 0; i < dd->nold_caches; i++) dd->vk.vkDestroyPipelineCache(dd->device, dd->old_caches[i], NULL);
    dd->pcache = VK_NULL_HANDLE;
    dd->nold_caches = 0;
    for (uint32_t i = 0; i < dd->layout_slots.used; i++)
        if (dd->layout_slots.live[i]) {
            gpu_vk_layout* const l = &dd->layout[i];
            dd->vk.vkDestroyPipelineLayout(dd->device, l->pl, NULL);
            dd->vk.vkDestroyDescriptorSetLayout(dd->device, l->set0, NULL);
            for (uint32_t k = 0; k < l->nstatics; k++) dd->vk.vkDestroySampler(dd->device, l->statics[k], NULL);
        }
}

#endif /* PS3RECOMP_RSX_HAL && PS3RECOMP_RSX_HAL_VULKAN */
