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
    gpu_vk_lock(&dd->lock);
    r.id = gpu_vk_slot_alloc(&dd->shader_slots);
    if (r.id) {
        gpu_vk_shader* const s = &dd->shader[GPU_HANDLE_INDEX(r.id)];
        s->module = m;
        s->stage = desc->stage;
        s->hash = gpu_vk_fnv(GPU_VK_FNV_BASIS, desc->bytes, (size_t)desc->size);
        snprintf(s->entry, sizeof s->entry, "%s", desc->entry ? desc->entry : "main");
    }
    gpu_vk_unlock(&dd->lock);
    if (!r.id) dd->vk.vkDestroyShaderModule(dd->device, m, NULL);
    return r;
}

void gpu_vk_shader_destroy(gpu_device* d, gpu_shader s)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    VkShaderModule m = VK_NULL_HANDLE;
    gpu_vk_lock(&dd->lock);
    if (gpu_vk_slot_valid(&dd->shader_slots, s.id)) {
        gpu_vk_shader* const sh = &dd->shader[GPU_HANDLE_INDEX(s.id)];
        m = sh->module;
        memset(sh, 0, sizeof *sh);
        gpu_vk_slot_free(&dd->shader_slots, s.id);
    }
    gpu_vk_unlock(&dd->lock);
    if (m) dd->vk.vkDestroyShaderModule(dd->device, m, NULL);
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
 * description's (set_vb's is not used). */
gpu_pipeline gpu_vk_pipeline_create(gpu_device* d, const gpu_pipeline_desc* desc, uint32_t* from_cache)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_pipeline r = {0};
    if (from_cache) *from_cache = 0;
    VkPipelineShaderStageCreateInfo st[3];
    uint32_t nst = 0;
    const gpu_shader sh[3] = {desc->vs, desc->gs, desc->ps};
    const VkShaderStageFlagBits stage[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                                            VK_SHADER_STAGE_FRAGMENT_BIT};
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipelineCache cache;
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
        for (uint32_t k = 0; k < desc->vertex.count; k++) {
            const gpu_vk_format* const f = gpu_vk_format_of(desc->vertex.attr[k].format, 0);
            if (!f) return r;
            va[k].location = k;
            va[k].binding = 0;
            va[k].format = f->vk;
            va[k].offset = desc->vertex.attr[k].offset;
        }
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = desc->vertex.count;
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
    gpu_vk_lock(&dd->pso_lock);
    cache = dd->pcache;
    gpu_vk_unlock(&dd->pso_lock);
    VkPipeline p = VK_NULL_HANDLE;
    if (dd->vk.vkCreateGraphicsPipelines(dd->device, cache, 1, &gi, NULL, &p) != VK_SUCCESS) return r;
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
    if (from_cache && (fb.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) &&
        (fb.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT))
        *from_cache = 1;
    r.v = (uint64_t)idx + 1u;
    return r;
}

/* ---------------------------------------------------------------------------
 * The pipeline cache's file
 * -------------------------------------------------------------------------*/

/* A header names the driver the data is for (Vulkan's own header says so
 * too, but a file is checked before the driver sees it). */
typedef struct pcache_head {
    char     magic[8];        /* "PS3VKPC1" */
    uint32_t vendor, device, driver;
    uint8_t  uuid[VK_UUID_SIZE];
    uint64_t size, fnv;
} pcache_head;

/* 1 when the file was this driver's and now seeds the cache. The old cache
 * is kept until the device goes (a pipeline worker may be using it). */
int gpu_vk_pipeline_cache_load(gpu_device* d, const char* path)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    FILE* f = path ? fopen(path, "rb") : NULL;
    pcache_head h;
    if (!f) return 0;
    void* data = NULL;
    int ok = fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "PS3VKPC1", 8) && h.vendor == dd->props.vendorID &&
             h.device == dd->props.deviceID && h.driver == dd->props.driverVersion &&
             !memcmp(h.uuid, dd->props.pipelineCacheUUID, VK_UUID_SIZE) && h.size && h.size < (1ull << 31);
    if (ok) {
        data = malloc((size_t)h.size);
        ok = data && fread(data, (size_t)h.size, 1, f) == 1 && gpu_vk_fnv(GPU_VK_FNV_BASIS, data, (size_t)h.size) == h.fnv;
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
    if (!ok) return 0;
    gpu_vk_lock(&dd->pso_lock);
    if (dd->nold_caches < 8) {
        dd->old_caches[dd->nold_caches++] = dd->pcache;
        dd->pcache = nc;
        nc = VK_NULL_HANDLE;
    }
    gpu_vk_unlock(&dd->pso_lock);
    if (nc) {   /* (eight loads already: keep the cache there is) */
        dd->vk.vkDestroyPipelineCache(dd->device, nc, NULL);
        return 0;
    }
    return 1;
}

/* Written to path.tmp, then moved over path, so a crash leaves the old file. */
int gpu_vk_pipeline_cache_save(gpu_device* d, const char* path)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    if (!path) return 0;
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
    memcpy(h.magic, "PS3VKPC1", 8);
    h.vendor = dd->props.vendorID;
    h.device = dd->props.deviceID;
    h.driver = dd->props.driverVersion;
    memcpy(h.uuid, dd->props.pipelineCacheUUID, VK_UUID_SIZE);
    h.size = size;
    h.fnv = gpu_vk_fnv(GPU_VK_FNV_BASIS, data, size);
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
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
