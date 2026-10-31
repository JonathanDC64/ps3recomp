/*
 * ps3recomp - the spv target's compiler: glslang's HLSL front end (gpu_shader_spv.h)
 *
 * The text (rsxc_shader.c's, after the edits) goes through glslang as
 * glslangValidator would take it with
 *
 *   -D -V --target-env vulkan1.1 -S <vert|frag|geom> -e main [--source-entrypoint <entry>]
 *   --shift-texture-binding 16 --shift-sampler-binding 48 --shift-ssbo-binding 16
 *   --auto-map-locations --invert-y (vertex shaders only) --hlsl-dx-position-w
 *   --nan-clamp
 *
 * so SPIR-V 1.3 for a Vulkan 1.1 client, with SPIRV-Tools' HLSL legalization
 * passes (glslang's default for HLSL; GPU_SC_OPTIMIZE adds its performance
 * passes). glslang's front end has no HLSL version switch: it keeps HLSL
 * 2018's rules (a vector ?: selects per component), which every text needs.
 * --nan-clamp makes min, max and clamp D3D's (a NaN operand gives the
 * other). The shifts make a binding say its register (b n, t 16 + n, s
 * 48 + n), which the layout pass (gpu_spv_apply_layout) turns into the
 * Vulkan backend's set and binding; without --auto-map-bindings a resource
 * with no register has no binding, and the layout pass refuses it. Only
 * vertex shaders flip y: a geometry shader passes the flipped position on.
 * glslang is set up once per process; compiles may run on any number of
 * threads at once.
 *
 * Built with PS3RECOMP_RSX_HAL and PS3RECOMP_RSX_HAL_SPV.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(PS3RECOMP_RSX_HAL_SPV)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>
#include <string>
#include <vector>

/* As glslang is built (ENABLE_HLSL, the front end this uses): the header
 * declares its HLSL-only calls under it. It adds functions, no data. */
#ifndef ENABLE_HLSL
#define ENABLE_HLSL 1
#endif
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>   /* (the source tree's layout: the pinned glslang is built here) */
#include <glslang/build_info.h>
#include <spirv-tools/libspirv.h>

#include "gpu_shader_spv.h"

namespace {

std::once_flag s_glslang_once;
bool s_glslang_ok;

void put_err(char* err, size_t cap, const char* what, const char* detail)
{
    if (!err || !cap) return;
    snprintf(err, cap, "%s%s%s", what, detail && *detail ? ": " : "", detail ? detail : "");
}

/* Every flag that changes what glslang makes, as the cache key's tag says it. */
const char k_flags[] =
    "vk1.1,spv1.3,main,invert-y-vs,dx-position-w,nan-clamp,shift=t16:s48:ssbo16,auto-map-locations,layout";

std::once_flag s_tag_once;
char s_compiler[64];

void make_compiler()
{
    snprintf(s_compiler, sizeof s_compiler, "glslang@%d.%d.%d%s+spirv-tools@%s", GLSLANG_VERSION_MAJOR,
             GLSLANG_VERSION_MINOR, GLSLANG_VERSION_PATCH, GLSLANG_VERSION_FLAVOR, spvSoftwareVersionString());
}

uint64_t fnv(uint64_t h, const char* s)
{
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
    return h;
}

/* What of a layout the module depends on: each slot's kind, register and
 * count, and the static samplers' registers (not visibility, flags or the
 * samplers' state). FNV-1a 64 over it as text. */
uint64_t layout_hash(const gpu_layout_desc* L)
{
    char buf[32];
    uint64_t h = fnv(14695981039346656037ull, "slots");
    for (uint32_t i = 0; i < L->nslots && i < GPU_MAX_SLOTS; i++) {
        snprintf(buf, sizeof buf, ";%u:%u:%u", L->slot[i].kind, L->slot[i].reg, L->slot[i].count);
        h = fnv(h, buf);
    }
    h = fnv(h, ";statics");
    for (uint32_t i = 0; i < L->nstatic && i < GPU_MAX_STATIC_SAMPLERS; i++) {
        snprintf(buf, sizeof buf, ";%u", L->statics[i].reg);
        h = fnv(h, buf);
    }
    return h;
}

} // namespace

extern "C" const char* gpu_spv_compiler(void)
{
    std::call_once(s_tag_once, make_compiler);
    return s_compiler;
}

extern "C" const char* gpu_spv_tag(const gpu_layout_desc* layout, char* buf, size_t cap)
{
    if (!buf || !cap) return "";
    snprintf(buf, cap, "spv;%s;%s;edits=%d.%d.%d.%d.%d;layout=%016llx", gpu_spv_compiler(), k_flags,
             GPU_SPV_E1_VERSION, GPU_SPV_E2_VERSION, GPU_SPV_E3_VERSION, GPU_SPV_E4_VERSION, GPU_SPV_E5_VERSION,
             (unsigned long long)layout_hash(layout ? layout : gpu_spv_guest_layout()));
    return buf;
}

extern "C" uint64_t gpu_spv_key(uint64_t k, const gpu_layout_desc* layout)
{
    char tag[320], hex[24];
    snprintf(hex, sizeof hex, ";%016llx", (unsigned long long)k);
    return fnv(fnv(14695981039346656037ull, gpu_spv_tag(layout, tag, sizeof tag)), hex);
}

extern "C" int gpu_spv_glslang(gpu_sc_stage stage, const char* text, size_t n, const char* name, const char* entry,
                               unsigned gl_flags, const gpu_layout_desc* layout, uint32_t** words, size_t* nwords,
                               char* err, size_t err_cap)
{
    if (words) *words = NULL;
    if (nwords) *nwords = 0;
    if (err && err_cap) err[0] = 0;
    if (!text || !words || !nwords || n > 0x7FFFFFFF || (unsigned)stage >= GPU_SC_STAGE_COUNT) return GPU_SC_BAD_ARG;
    std::call_once(s_glslang_once, [] { s_glslang_ok = glslang::InitializeProcess(); });
    if (!s_glslang_ok) { put_err(err, err_cap, "glslang::InitializeProcess failed", NULL); return GPU_SC_FAILED; }

    const EShLanguage lang = stage == GPU_SC_VS ? EShLangVertex : stage == GPU_SC_PS ? EShLangFragment
                                                                                     : EShLangGeometry;
    const EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl);
    glslang::TShader sh(lang);
    const char* strings[1] = { text };
    const int lengths[1] = { (int)n };
    const char* names[1] = { name && *name ? name : "spv" };
    sh.setStringsWithLengthsAndNames(strings, lengths, names, 1);
    /* The module's entry point is always "main"; another source function is renamed to it. */
    sh.setEntryPoint("main");
    if (entry && strcmp(entry, "main")) sh.setSourceEntryPoint(entry);
    sh.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    sh.setShiftBinding(glslang::EResTexture, 16);
    sh.setShiftBinding(glslang::EResSampler, 48);
    sh.setShiftBinding(glslang::EResSsbo, 16);
    sh.setAutoMapLocations(true);
    if (lang == EShLangVertex) sh.setInvertY(true);
    sh.setDxPositionW(true);
    sh.setNanMinMaxClamp(true);
    if (gl_flags & GPU_SPV_GL_SEMANTICS) sh.setEnvTargetHlslFunctionality1();
    if (!sh.parse(GetDefaultResources(), 100, false, msgs)) {
        put_err(err, err_cap, "HLSL parse", sh.getInfoLog());
        return GPU_SC_FAILED;
    }
    glslang::TProgram prog;
    prog.addShader(&sh);
    if (!prog.link(msgs)) {
        put_err(err, err_cap, "HLSL link", prog.getInfoLog());
        return GPU_SC_FAILED;
    }
    if (!prog.mapIO()) {
        put_err(err, err_cap, "HLSL io map", prog.getInfoLog());
        return GPU_SC_FAILED;
    }
    std::vector<unsigned int> spv;
    spv::SpvBuildLogger logger;
    glslang::SpvOptions so;
    so.disableOptimizer = false;          /* the HLSL legalization passes */
    so.optimizePerformance = (gl_flags & GPU_SPV_GL_OPTIMIZE) != 0;
    glslang::GlslangToSpv(*prog.getIntermediate(lang), spv, &logger, &so);
    if (spv.empty()) {
        put_err(err, err_cap, "SPIR-V generation", logger.getAllMessages().c_str());
        return GPU_SC_FAILED;
    }
    uint32_t* w = (uint32_t*)malloc(spv.size() * sizeof *w);
    if (!w) { put_err(err, err_cap, "out of memory", NULL); return GPU_SC_FAILED; }
    memcpy(w, spv.data(), spv.size() * sizeof *w);
    if (layout && gpu_spv_apply_layout(w, spv.size(), layout, err, err_cap) < 0) {
        free(w);
        if (err && err_cap && !err[0]) put_err(err, err_cap, "layout: malformed module", NULL);
        return GPU_SC_FAILED;
    }
    *words = w;
    *nwords = spv.size();
    return GPU_SC_OK;
}

extern "C" int gpu_spv_validate(const uint32_t* words, size_t nwords, char* err, size_t err_cap)
{
    if (err && err_cap) err[0] = 0;
    if (!words || !nwords) { put_err(err, err_cap, "no module", NULL); return 0; }
    spv_context ctx = spvContextCreate(SPV_ENV_VULKAN_1_1);
    if (!ctx) { put_err(err, err_cap, "spvContextCreate failed", NULL); return 0; }
    spv_validator_options vo = spvValidatorOptionsCreate();
    spv_const_binary_t bin = { words, nwords };
    spv_diagnostic diag = NULL;
    const spv_result_t r = spvValidateWithOptions(ctx, vo, &bin, &diag);
    if (r != SPV_SUCCESS)
        put_err(err, err_cap, "spirv-val", diag && diag->error ? diag->error : "invalid module");
    spvDiagnosticDestroy(diag);
    spvValidatorOptionsDestroy(vo);
    spvContextDestroy(ctx);
    return r == SPV_SUCCESS;
}

#else
typedef int gpu_shader_spv_off;
#endif /* PS3RECOMP_RSX_HAL && PS3RECOMP_RSX_HAL_SPV */
