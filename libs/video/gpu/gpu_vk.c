/*
 * ps3recomp - the portable renderer's GPU layer: the Vulkan backend
 *
 * The device: the instance (through volk), the physical device, the logical
 * device and its feature bits, the handle tables, the recorders, the queue
 * and its timeline, and the recording helpers the inline commands
 * (gpu_vk_cmd.h) call when there is more to do than one call.
 *
 * Vulkan 1.3 with dynamic rendering, synchronization2 and timeline
 * semaphores is required; the rest are feature bits with fallbacks
 * (gpu.h's GPU_CAP_*). RSX_VK_MASK=<bits> turns feature bits off, to try
 * their fallbacks: the features behind them are not even enabled.
 * RSX_VK_VALIDATION=1 loads the validation layer, with synchronization
 * validation, and counts what it says (the tests fail on any message).
 *
 * Built with PS3RECOMP_RSX_HAL and PS3RECOMP_RSX_HAL_VULKAN.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(PS3RECOMP_RSX_HAL_VULKAN)

#include "gpu_vk_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Handle slots (gpu_d3d12.c's rules)
 * -------------------------------------------------------------------------*/

static int slots_init(gpu_vk_slots* s, uint32_t cap)
{
    s->cap = cap;
    s->used = s->nfree = 0;
    s->free = (uint32_t*)malloc(cap * sizeof(uint32_t));
    s->gen = (uint8_t*)calloc(cap, 1);
    s->live = (uint8_t*)calloc(cap, 1);
    return s->free && s->gen && s->live;
}

static void slots_fini(gpu_vk_slots* s)
{
    free(s->free);
    free(s->gen);
    free(s->live);
    memset(s, 0, sizeof *s);
}

int gpu_vk_slot_valid(const gpu_vk_slots* s, uint32_t id)
{
    const uint32_t i = GPU_HANDLE_INDEX(id);
    return id && i < s->used && s->live[i] && s->gen[i] == GPU_HANDLE_GEN(id);
}

uint32_t gpu_vk_slot_alloc(gpu_vk_slots* s)
{
    uint32_t i;
    if (s->nfree) i = s->free[--s->nfree];
    else if (s->used < s->cap) i = s->used++;
    else return 0;
    s->live[i] = 1;
    return GPU_HANDLE_MAKE(i, s->gen[i]);
}

/* Retired at generation 255, so a handle names one object for good. */
void gpu_vk_slot_free(gpu_vk_slots* s, uint32_t id)
{
    const uint32_t i = GPU_HANDLE_INDEX(id);
    s->live[i] = 0;
    if (s->gen[i] == 255) return;
    s->gen[i]++;
    s->free[s->nfree++] = i;
}

uint32_t gpu_vk_lost_note(gpu_vk_device* dd, VkResult r)
{
    if (r == VK_ERROR_DEVICE_LOST) gpu_vk_cas32(&dd->lost, (uint32_t)r, 0);
    return (uint32_t)r;
}

uint32_t gpu_vk_mem_type(const gpu_vk_device* dd, uint32_t bits, VkMemoryPropertyFlags want,
                         VkMemoryPropertyFlags avoid)
{
    for (uint32_t i = 0; i < dd->mem.memoryTypeCount; i++) {
        const VkMemoryPropertyFlags f = dd->mem.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & want) == want && !(f & avoid)) return i;
    }
    return UINT32_MAX;
}

void gpu_vk_name(gpu_vk_device* dd, VkObjectType type, uint64_t handle, const char* name)
{
    if (!name || !dd->has_debug_utils || !vkSetDebugUtilsObjectNameEXT) return;
    VkDebugUtilsObjectNameInfoEXT ni;
    memset(&ni, 0, sizeof ni);
    ni.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    ni.objectType = type;
    ni.objectHandle = handle;
    ni.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(dd->device, &ni);
}

static uint32_t env_u32(const char* name, uint32_t dflt)
{
    const char* v = getenv(name);
    return v && *v ? (uint32_t)strtoul(v, NULL, 0) : dflt;
}

/* ---------------------------------------------------------------------------
 * The instance
 * -------------------------------------------------------------------------*/

typedef struct name_list {
    const char* name[48];
    uint32_t    n;
} name_list;

static void list_add(name_list* l, const char* name)
{
    if (l->n < 48) l->name[l->n++] = name;
}

static int ext_has(const VkExtensionProperties* p, uint32_t n, const char* name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(p[i].extensionName, name)) return 1;
    return 0;
}

static VkExtensionProperties* inst_exts(const char* layer, uint32_t* n)
{
    *n = 0;
    if (vkEnumerateInstanceExtensionProperties(layer, n, NULL) != VK_SUCCESS) return NULL;
    VkExtensionProperties* p = (VkExtensionProperties*)calloc(*n ? *n : 1, sizeof *p);
    if (p && vkEnumerateInstanceExtensionProperties(layer, n, p) != VK_SUCCESS) *n = 0;
    return p;
}

/* The validation layer's messages (validation and performance) are printed
 * and counted: the tests fail on any. The loader's own (general) speak of
 * the machine's layers: only its errors are printed. */
volatile uint32_t gpu_vk_n_validation_all;
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                               VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
{
    gpu_vk_device* const dd = (gpu_vk_device*)user;
    if (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) {
        gpu_vk_add32(&dd->n_validation);
        gpu_vk_add32(&gpu_vk_n_validation_all);
    } else if (!(sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) {
        return VK_FALSE;   /* the loader's warnings (layers turned off, registry keys): quiet */
    }
    fprintf(stderr, "[VK] %s: %s\n", (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "error" : "warning",
            data && data->pMessageIdName ? data->pMessageIdName : "");
    fprintf(stderr, "     %s\n", data && data->pMessage ? data->pMessage : "");
    return VK_FALSE;
}

static void messenger_info(gpu_vk_device* dd, VkDebugUtilsMessengerCreateInfoEXT* mi)
{
    memset(mi, 0, sizeof *mi);
    mi->sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    mi->messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mi->messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mi->pfnUserCallback = debug_cb;
    mi->pUserData = dd;
}

static int volk_ready(void)
{
    static int done = 0;   /* (the library stays loaded for the process) */
    if (!done && volkInitialize() == VK_SUCCESS) done = 1;
    return done;
}

static int instance_make(gpu_vk_device* dd, int validation)
{
    if (!volk_ready()) {
        printf("[VK] no Vulkan library\n");
        return 0;
    }
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        printf("[VK] the Vulkan library is older than 1.3\n");
        return 0;
    }
    uint32_t n = 0, nl = 0;
    VkExtensionProperties* ep = inst_exts(NULL, &n);
    VkExtensionProperties* lp = NULL;
    name_list ext;
    memset(&ext, 0, sizeof ext);

    /* Surfaces: VK_KHR_surface and the platform's (the first is required). */
    {
        const char* const* se = gpu_vk_surface_extensions();
        uint32_t platform = 0;
        if (ext_has(ep, n, VK_KHR_SURFACE_EXTENSION_NAME)) {
            list_add(&ext, VK_KHR_SURFACE_EXTENSION_NAME);
            for (uint32_t i = 0; se[i]; i++)
                if (ext_has(ep, n, se[i])) {
                    list_add(&ext, se[i]);
                    platform++;
                }
        }
        dd->has_surface = platform != 0;
    }
    if (dd->has_surface && ext_has(ep, n, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
        list_add(&ext, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        dd->has_colorspace = 1;
    }
    if (dd->has_surface && ext_has(ep, n, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
        list_add(&ext, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        dd->has_surface_caps2 = 1;
        if (ext_has(ep, n, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME)) {
            list_add(&ext, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
            dd->has_surface_maint1 = 2;
        } else if (ext_has(ep, n, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME)) {
            list_add(&ext, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
            dd->has_surface_maint1 = 1;
        }
    }

    /* The validation layer, with synchronization validation when the layer
     * takes settings. */
    const char* layer = "VK_LAYER_KHRONOS_validation";
    int settings = 0;
    if (validation) {
        uint32_t nlay = 0;
        vkEnumerateInstanceLayerProperties(&nlay, NULL);
        VkLayerProperties* lay = (VkLayerProperties*)calloc(nlay ? nlay : 1, sizeof *lay);
        int found = 0;
        if (lay && vkEnumerateInstanceLayerProperties(&nlay, lay) == VK_SUCCESS)
            for (uint32_t i = 0; i < nlay; i++)
                if (!strcmp(lay[i].layerName, layer)) found = 1;
        free(lay);
        if (!found) {
            printf("[VK] RSX_VK_VALIDATION: no %s (VK_ADD_LAYER_PATH)\n", layer);
            validation = 0;
        } else {
            lp = inst_exts(layer, &nl);
            if (ext_has(lp, nl, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME)) {
                list_add(&ext, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
                settings = 1;
            }
        }
    }
    if (validation && (ext_has(ep, n, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) ||
                       ext_has(lp, nl, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))) {
        list_add(&ext, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        dd->has_debug_utils = 1;
    }
    free(ep);
    free(lp);

    VkApplicationInfo app;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "ps3recomp";
    app.pEngineName = "ps3recomp gpu";
    app.apiVersion = VK_API_VERSION_1_3;

    VkDebugUtilsMessengerCreateInfoEXT mi;
    messenger_info(dd, &mi);
    const VkBool32 on = VK_TRUE;
    VkLayerSettingEXT ls;
    ls.pLayerName = layer;
    ls.pSettingName = "validate_sync";
    ls.type = VK_LAYER_SETTING_TYPE_BOOL32_EXT;
    ls.valueCount = 1;
    ls.pValues = &on;
    VkLayerSettingsCreateInfoEXT lsi;
    memset(&lsi, 0, sizeof lsi);
    lsi.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
    lsi.settingCount = 1;
    lsi.pSettings = &ls;

    VkInstanceCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = ext.n;
    ci.ppEnabledExtensionNames = ext.name;
    if (validation) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layer;
        if (dd->has_debug_utils) {
            mi.pNext = settings ? &lsi : NULL;
            ci.pNext = &mi;
        } else if (settings) {
            ci.pNext = &lsi;
        }
    }
    const VkResult r = vkCreateInstance(&ci, NULL, &dd->instance);
    if (r != VK_SUCCESS) {
        printf("[VK] vkCreateInstance failed (%d)\n", (int)r);
        dd->instance = VK_NULL_HANDLE;
        return 0;
    }
    volkLoadInstanceOnly(dd->instance);
    dd->validation = (uint32_t)validation;
    if (dd->has_debug_utils) {
        mi.pNext = NULL;
        if (vkCreateDebugUtilsMessengerEXT(dd->instance, &mi, NULL, &dd->messenger) != VK_SUCCESS)
            dd->messenger = VK_NULL_HANDLE;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * The physical device
 * -------------------------------------------------------------------------*/

/* What every device must have: Vulkan 1.3, its dynamic rendering and
 * synchronization2, timeline semaphores, partly bound descriptor sets whose
 * unused entries may be written while in flight (D3D12's tables), and a
 * graphics queue. */
static int phys_ok(VkPhysicalDevice p, uint32_t* qfamily)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(p, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) return 0;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceFeatures2 f2;
    memset(&f12, 0, sizeof f12);
    memset(&f13, 0, sizeof f13);
    memset(&f2, 0, sizeof f2);
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f12;
    f12.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(p, &f2);
    if (!f12.timelineSemaphore || !f12.descriptorBindingPartiallyBound ||
        !f12.descriptorBindingUpdateUnusedWhilePending || !f13.dynamicRendering || !f13.synchronization2)
        return 0;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &nq, NULL);
    VkQueueFamilyProperties q[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &nq, q);
    for (uint32_t i = 0; i < nq; i++)
        if ((q[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            *qfamily = i;
            return 1;
        }
    return 0;
}

/* GPU_DEVICE_WARP: a CPU device (lavapipe). GPU_DEVICE_USE_LUID: the device
 * DXGI names by that luid. Else the discrete GPU first (the integrated one
 * first with GPU_DEVICE_LOW_POWER), a CPU device last. */
static int phys_pick(gpu_vk_device* dd, const gpu_device_desc* desc)
{
    uint32_t n = 0;
    if (vkEnumeratePhysicalDevices(dd->instance, &n, NULL) != VK_SUCCESS || !n) {
        printf("[VK] no Vulkan device\n");
        return 0;
    }
    VkPhysicalDevice* list = (VkPhysicalDevice*)calloc(n, sizeof *list);
    if (!list) return 0;
    vkEnumeratePhysicalDevices(dd->instance, &n, list);
    int best = -1;
    uint32_t best_rank = 99, best_q = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t qf = 0;
        if (!phys_ok(list[i], &qf)) continue;
        VkPhysicalDeviceIDProperties id;
        VkPhysicalDeviceProperties2 p2;
        memset(&id, 0, sizeof id);
        memset(&p2, 0, sizeof p2);
        id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &id;
        vkGetPhysicalDeviceProperties2(list[i], &p2);
        const VkPhysicalDeviceType t = p2.properties.deviceType;
        uint32_t rank;
        if (desc->flags & GPU_DEVICE_WARP) {
            if (t != VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
            rank = 0;
        } else if (desc->flags & GPU_DEVICE_USE_LUID) {
            if (!id.deviceLUIDValid || memcmp(id.deviceLUID, desc->luid, 8)) continue;
            rank = 0;
        } else if (t == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            rank = 9;
        } else if (t == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            rank = (desc->flags & GPU_DEVICE_LOW_POWER) ? 1 : 0;
        } else if (t == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            rank = (desc->flags & GPU_DEVICE_LOW_POWER) ? 0 : 1;
        } else {
            rank = 2;
        }
        if (rank < best_rank) {
            best_rank = rank;
            best = (int)i;
            best_q = qf;
        }
    }
    if (best >= 0) {
        dd->phys = list[best];
        dd->qfamily = best_q;
    }
    free(list);
    if (best < 0) printf("[VK] no Vulkan 1.3 device with the features the renderer needs\n");
    return best >= 0;
}

/* ---------------------------------------------------------------------------
 * The device, its features and feature bits
 * -------------------------------------------------------------------------*/

static int device_make(gpu_vk_device* dd, const gpu_device_desc* desc, uint32_t mask)
{
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(dd->phys, NULL, &n, NULL);
    VkExtensionProperties* ep = (VkExtensionProperties*)calloc(n ? n : 1, sizeof *ep);
    if (!ep) return 0;
    vkEnumerateDeviceExtensionProperties(dd->phys, NULL, &n, ep);

    /* What the device has: the structs of the extensions it lists. */
    VkPhysicalDeviceFeatures2 a2;
    VkPhysicalDeviceVulkan11Features a11;
    VkPhysicalDeviceVulkan12Features a12;
    VkPhysicalDeviceVulkan13Features a13;
    VkPhysicalDeviceRobustness2FeaturesKHR ar2;
    VkPhysicalDevicePresentIdFeaturesKHR apid;
    VkPhysicalDevicePresentWaitFeaturesKHR apw;
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR asm1;
    VkPhysicalDeviceFaultFeaturesEXT afault;
    VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT aua;
    memset(&a2, 0, sizeof a2);
    memset(&a11, 0, sizeof a11);
    memset(&a12, 0, sizeof a12);
    memset(&a13, 0, sizeof a13);
    memset(&ar2, 0, sizeof ar2);
    memset(&apid, 0, sizeof apid);
    memset(&apw, 0, sizeof apw);
    memset(&asm1, 0, sizeof asm1);
    memset(&afault, 0, sizeof afault);
    memset(&aua, 0, sizeof aua);
    a2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    a11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    a12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    a13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    ar2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR;
    apid.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    apw.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    asm1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
    afault.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
    aua.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT;
    const int e_r2k = ext_has(ep, n, VK_KHR_ROBUSTNESS_2_EXTENSION_NAME);
    const int e_r2 = e_r2k || ext_has(ep, n, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    const int e_swap = dd->has_surface && ext_has(ep, n, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    const int e_pid = e_swap && ext_has(ep, n, VK_KHR_PRESENT_ID_EXTENSION_NAME);
    const int e_pw = e_pid && ext_has(ep, n, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    const int e_sm1k = e_swap && dd->has_surface_maint1 == 2 && ext_has(ep, n, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    const int e_sm1 = e_sm1k || (e_swap && dd->has_surface_maint1 &&
                                 ext_has(ep, n, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME));
    const int e_fault = ext_has(ep, n, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    const int e_ua = ext_has(ep, n, VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME);
    const int e_host = ext_has(ep, n, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    const int e_cp = ext_has(ep, n, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    const int e_hdr = e_swap && ext_has(ep, n, VK_EXT_HDR_METADATA_EXTENSION_NAME);
    free(ep);
    {
        void** tail = &a2.pNext;
#define CHAIN(cond, s) do { if (cond) { *tail = &(s); tail = &(s).pNext; } } while (0)
        CHAIN(1, a11);
        CHAIN(1, a12);
        CHAIN(1, a13);
        CHAIN(e_r2, ar2);
        CHAIN(e_pid, apid);
        CHAIN(e_pw, apw);
        CHAIN(e_sm1, asm1);
        CHAIN(e_fault, afault);
        CHAIN(e_ua, aua);
        vkGetPhysicalDeviceFeatures2(dd->phys, &a2);
    }

    /* What is enabled: each optional feature unless RSX_VK_MASK names it. */
    VkPhysicalDeviceFeatures2 f2;
    VkPhysicalDeviceVulkan11Features f11;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceRobustness2FeaturesKHR r2;
    VkPhysicalDevicePresentIdFeaturesKHR pid;
    VkPhysicalDevicePresentWaitFeaturesKHR pw;
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR sm1;
    VkPhysicalDeviceFaultFeaturesEXT fault;
    VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT ua;
    memset(&f2, 0, sizeof f2);
    memset(&f11, 0, sizeof f11);
    memset(&f12, 0, sizeof f12);
    memset(&f13, 0, sizeof f13);
    memset(&r2, 0, sizeof r2);
    memset(&pid, 0, sizeof pid);
    memset(&pw, 0, sizeof pw);
    memset(&sm1, 0, sizeof sm1);
    memset(&fault, 0, sizeof fault);
    memset(&ua, 0, sizeof ua);
    f2.sType = a2.sType;
    f11.sType = a11.sType;
    f12.sType = a12.sType;
    f13.sType = a13.sType;
    r2.sType = ar2.sType;
    pid.sType = apid.sType;
    pw.sType = apw.sType;
    sm1.sType = asm1.sType;
    fault.sType = afault.sType;
    ua.sType = aua.sType;
    const VkPhysicalDeviceFeatures* const af = &a2.features;
    VkPhysicalDeviceFeatures* const ff = &f2.features;
#define OPT(x, bit) ((x) && !(mask & (bit)))
    ff->geometryShader = OPT(af->geometryShader, GPU_CAP_GS);
    ff->depthClamp = OPT(af->depthClamp, GPU_CAP_DEPTH_CLAMP);
    ff->samplerAnisotropy = OPT(af->samplerAnisotropy, GPU_CAP_ANISO16);
    ff->occlusionQueryPrecise = OPT(af->occlusionQueryPrecise, GPU_CAP_OCCLUSION_PRECISE);
    ff->textureCompressionBC = OPT(af->textureCompressionBC, GPU_CAP_BC);
    ff->robustBufferAccess = OPT(af->robustBufferAccess, GPU_CAP_ROBUST2);
    ff->independentBlend = af->independentBlend;
    ff->dualSrcBlend = af->dualSrcBlend;
    ff->imageCubeArray = af->imageCubeArray;
    ff->shaderClipDistance = af->shaderClipDistance;
    ff->shaderCullDistance = af->shaderCullDistance;
    ff->fragmentStoresAndAtomics = af->fragmentStoresAndAtomics;
    f11.shaderDrawParameters = a11.shaderDrawParameters;
    f12.timelineSemaphore = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    f12.samplerMirrorClampToEdge = OPT(a12.samplerMirrorClampToEdge, GPU_CAP_MIRROR_ONCE);
    f12.hostQueryReset = a12.hostQueryReset;
    f12.scalarBlockLayout = a12.scalarBlockLayout;
    f12.uniformBufferStandardLayout = a12.uniformBufferStandardLayout;
    f12.shaderFloat16 = a12.shaderFloat16;
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.maintenance4 = a13.maintenance4;
    f13.shaderDemoteToHelperInvocation = a13.shaderDemoteToHelperInvocation;
    r2.robustBufferAccess2 = ff->robustBufferAccess && OPT(ar2.robustBufferAccess2, GPU_CAP_ROBUST2);
    r2.nullDescriptor = OPT(ar2.nullDescriptor, GPU_CAP_NULL_DESCRIPTOR);
    pid.presentId = OPT(apid.presentId, GPU_CAP_PRESENT_WAIT);
    pw.presentWait = pid.presentId && OPT(apw.presentWait, GPU_CAP_PRESENT_WAIT);
    sm1.swapchainMaintenance1 = OPT(asm1.swapchainMaintenance1, GPU_CAP_PRESENT_MODE_SWITCH);
    fault.deviceFault = OPT(afault.deviceFault, GPU_CAP_DEVICE_FAULT);
    ua.dynamicRenderingUnusedAttachments = OPT(aua.dynamicRenderingUnusedAttachments, GPU_CAP_UNUSED_ATTACHMENTS);
    const int use_cp = e_cp && (desc->flags & GPU_DEVICE_BREADCRUMBS) && !(mask & GPU_CAP_BREADCRUMBS);
    const int use_host = e_host && !(mask & GPU_CAP_HOST_IMPORT);
    const int use_hdr = e_hdr && !(mask & GPU_CAP_HDR_METADATA);

    name_list ext;
    memset(&ext, 0, sizeof ext);
    void** tail = &f2.pNext;
    CHAIN(1, f11);
    CHAIN(1, f12);
    CHAIN(1, f13);
    if (e_swap) list_add(&ext, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (e_r2 && (r2.robustBufferAccess2 || r2.nullDescriptor)) {
        list_add(&ext, e_r2k ? VK_KHR_ROBUSTNESS_2_EXTENSION_NAME : VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
        CHAIN(1, r2);
    }
    if (pid.presentId) {
        list_add(&ext, VK_KHR_PRESENT_ID_EXTENSION_NAME);
        CHAIN(1, pid);
    }
    if (pw.presentWait) {
        list_add(&ext, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        CHAIN(1, pw);
    }
    if (sm1.swapchainMaintenance1) {
        list_add(&ext, e_sm1k ? VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME
                              : VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        CHAIN(1, sm1);
    }
    if (fault.deviceFault) {
        list_add(&ext, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        CHAIN(1, fault);
    }
    if (ua.dynamicRenderingUnusedAttachments) {
        list_add(&ext, VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME);
        CHAIN(1, ua);
    }
#undef CHAIN
    if (use_host) list_add(&ext, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    if (use_cp) list_add(&ext, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    if (use_hdr) list_add(&ext, VK_EXT_HDR_METADATA_EXTENSION_NAME);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi;
    memset(&qi, 0, sizeof qi);
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = dd->qfamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = ext.n;
    ci.ppEnabledExtensionNames = ext.name;
    const VkResult r = vkCreateDevice(dd->phys, &ci, NULL, &dd->device);
    if (r != VK_SUCCESS) {
        printf("[VK] vkCreateDevice failed (%d)\n", (int)r);
        dd->device = VK_NULL_HANDLE;
        return 0;
    }
    volkLoadDeviceTable(&dd->vk, dd->device);
    dd->vk.vkGetDeviceQueue(dd->device, dd->qfamily, 0, &dd->queue);

    dd->has_gs = ff->geometryShader;
    dd->has_depth_clamp = ff->depthClamp;
    dd->has_aniso = ff->samplerAnisotropy;
    dd->has_precise = ff->occlusionQueryPrecise;
    dd->has_bc = ff->textureCompressionBC;
    dd->has_independent_blend = ff->independentBlend;
    dd->has_dual_src = ff->dualSrcBlend;
    dd->has_mirror = f12.samplerMirrorClampToEdge;
    dd->has_host_reset = f12.hostQueryReset;
    dd->has_robust2 = r2.robustBufferAccess2;
    dd->has_robust2_khr = (uint32_t)e_r2k;
    dd->has_null_desc = r2.nullDescriptor;
    dd->has_present_id = pid.presentId;
    dd->has_present_wait = pw.presentWait;
    dd->has_swap_maint1 = sm1.swapchainMaintenance1;
    dd->has_maint1_khr = (uint32_t)(sm1.swapchainMaintenance1 && e_sm1k);
    dd->has_fault = fault.deviceFault;
    dd->has_unused_att = ua.dynamicRenderingUnusedAttachments;
    dd->has_host_import = (uint32_t)use_host;
    dd->has_checkpoints = (uint32_t)use_cp;
    dd->has_hdr_meta = (uint32_t)use_hdr;
    if (!e_swap) dd->has_surface = 0;
    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(dd->phys, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
        dd->has_d24s8 = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
                        !(mask & GPU_CAP_D24S8);
    }
#undef OPT
    return 1;
}

static void caps_make(gpu_vk_device* dd, uint32_t mask)
{
    gpu_caps* const c = &dd->base.caps;
    const VkPhysicalDeviceLimits* const lim = &dd->props.limits;
    uint32_t b = GPU_CAP_BORDER | GPU_CAP_SWIZZLE | GPU_CAP_PIPELINE_CACHE;
    if (dd->has_gs) b |= GPU_CAP_GS;
    if (dd->has_d24s8) b |= GPU_CAP_D24S8;
    if (dd->has_depth_clamp) b |= GPU_CAP_DEPTH_CLAMP;
    if (dd->has_mirror) b |= GPU_CAP_MIRROR_ONCE;
    if (dd->has_aniso && lim->maxSamplerAnisotropy >= 16.0f) b |= GPU_CAP_ANISO16;
    if (dd->has_bc) b |= GPU_CAP_BC;
    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(dd->phys, VK_FORMAT_R32G32B32A32_SFLOAT, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) b |= GPU_CAP_RGBA32F_BLEND;
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) b |= GPU_CAP_RGBA32F_FILTER;
    }
    if (dd->has_precise) b |= GPU_CAP_OCCLUSION_PRECISE;
    if (dd->has_host_import) b |= GPU_CAP_HOST_IMPORT;
    if (dd->has_null_desc) b |= GPU_CAP_NULL_DESCRIPTOR;
    if (dd->has_robust2) b |= GPU_CAP_ROBUST2;
    /* IMMEDIATE is a surface's, so assumed until the first swap chain says
     * (gpu_vk_swap_create clears it when the window cannot tear). */
    if (dd->has_surface) b |= GPU_CAP_TEARING;
    if (dd->has_swap_maint1) b |= GPU_CAP_PRESENT_MODE_SWITCH;
    if (dd->has_present_wait) b |= GPU_CAP_PRESENT_WAIT;
    if (dd->has_colorspace) b |= GPU_CAP_HDR_SCRGB;
    if (dd->has_hdr_meta) b |= GPU_CAP_HDR_METADATA;
    if (dd->has_fault) b |= GPU_CAP_DEVICE_FAULT;
    if (dd->has_checkpoints) b |= GPU_CAP_BREADCRUMBS;
    if (dd->has_unused_att) b |= GPU_CAP_UNUSED_ATTACHMENTS;
    b &= ~mask;
    c->api = GPU_API_VULKAN;
    c->bits = b;
    c->cb_align = lim->minUniformBufferOffsetAlignment > 256 ? (uint32_t)lim->minUniformBufferOffsetAlignment : 256u;
    if (lim->minStorageBufferOffsetAlignment > c->cb_align) c->cb_align = (uint32_t)lim->minStorageBufferOffsetAlignment;
    c->max_aniso = dd->has_aniso ? (lim->maxSamplerAnisotropy >= 16.0f ? 16u : (uint32_t)lim->maxSamplerAnisotropy) : 1u;
    c->host_import_align = dd->has_host_import ? (uint32_t)dd->host_align : 0u;
}

/* ---------------------------------------------------------------------------
 * Tables, recorders, the device's life
 * -------------------------------------------------------------------------*/

static int tables_init(gpu_vk_device* dd)
{
    int ok = slots_init(&dd->tex_slots, GPU_VK_MAX_TEXTURES) & slots_init(&dd->buf_slots, GPU_VK_MAX_BUFFERS) &
             slots_init(&dd->view_slots, GPU_VK_MAX_VIEWS) & slots_init(&dd->samp_slots, GPU_VK_MAX_SAMPLERS) &
             slots_init(&dd->shader_slots, GPU_VK_MAX_SHADERS) & slots_init(&dd->layout_slots, GPU_VK_MAX_LAYOUTS) &
             slots_init(&dd->qpool_slots, GPU_VK_MAX_QPOOLS) & slots_init(&dd->heap_slots, GPU_VK_MAX_HEAPS) &
             slots_init(&dd->tpool_slots, GPU_VK_MAX_TPOOLS) & slots_init(&dd->swap_slots, GPU_VK_MAX_SWAPS);
    dd->tex = (gpu_vk_tex*)calloc(GPU_VK_MAX_TEXTURES, sizeof *dd->tex);
    dd->tex_layout = (uint32_t*)calloc(GPU_VK_MAX_TEXTURES, sizeof *dd->tex_layout);
    dd->buf_vk = (VkBuffer*)calloc(GPU_VK_MAX_BUFFERS, sizeof *dd->buf_vk);
    dd->buf = (gpu_vk_buf*)calloc(GPU_VK_MAX_BUFFERS, sizeof *dd->buf);
    dd->view = (gpu_vk_view*)calloc(GPU_VK_MAX_VIEWS, sizeof *dd->view);
    dd->samp = (gpu_vk_sampler*)calloc(GPU_VK_MAX_SAMPLERS, sizeof *dd->samp);
    dd->shader = (gpu_vk_shader*)calloc(GPU_VK_MAX_SHADERS, sizeof *dd->shader);
    dd->layout = (gpu_vk_layout*)calloc(GPU_VK_MAX_LAYOUTS, sizeof *dd->layout);
    dd->heap = (gpu_vk_heap*)calloc(GPU_VK_MAX_HEAPS, sizeof *dd->heap);
    dd->pso = (gpu_vk_pso*)calloc(GPU_VK_MAX_PIPELINES, sizeof *dd->pso);
    return ok && dd->tex && dd->tex_layout && dd->buf_vk && dd->buf && dd->view && dd->samp && dd->shader &&
           dd->layout && dd->heap && dd->pso;
}

static void cmd_init(gpu_vk_device* dd, gpu_cmd* c)
{
    c->head.ops = &gpu_vk_cmd_ops;
    c->head.dev = &dd->base;
    c->vk = &dd->vk;
    c->dd = dd;
    c->tex = dd->tex;
    c->tex_layout = dd->tex_layout;
    c->buf = dd->buf_vk;
    c->bufi = dd->buf;
    c->view = dd->view;
    c->layouts = dd->layout;
    c->qpool = dd->qpool;
    c->pso = dd->pso;
}

static int pool_make(gpu_vk_device* dd, VkCommandPool* pool, VkCommandBuffer* cb, VkCommandPoolCreateFlags flags)
{
    VkCommandPoolCreateInfo pi;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = flags;
    pi.queueFamilyIndex = dd->qfamily;
    if (dd->vk.vkCreateCommandPool(dd->device, &pi, NULL, pool) != VK_SUCCESS) return 0;
    if (!cb) return 1;
    VkCommandBufferAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = *pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    return dd->vk.vkAllocateCommandBuffers(dd->device, &ai, cb) == VK_SUCCESS;
}

static void device_free(gpu_vk_device* dd)
{
    if (dd->device) {
        dd->vk.vkDeviceWaitIdle(dd->device);
        for (uint32_t i = 0; i < dd->swap_slots.used; i++)
            if (dd->swap_slots.live[i]) {
                gpu_swapchain sc;
                sc.id = GPU_HANDLE_MAKE(i, dd->swap_slots.gen[i]);
                gpu_vk_swap_destroy(&dd->base, sc);
            }
        gpu_vk_defer_collect(dd, 1);
        gpu_vk_lock(&dd->lock);
        for (uint32_t i = 0; i < dd->tex_slots.used; i++)
            if (dd->tex_slots.live[i]) gpu_vk_tex_free_now(dd, GPU_HANDLE_MAKE(i, dd->tex_slots.gen[i]));
        for (uint32_t i = 0; i < dd->buf_slots.used; i++)
            if (dd->buf_slots.live[i]) gpu_vk_buf_free_now(dd, GPU_HANDLE_MAKE(i, dd->buf_slots.gen[i]));
        gpu_vk_unlock(&dd->lock);
        for (uint32_t i = 0; i < dd->samp_slots.used; i++)
            if (dd->samp_slots.live[i]) dd->vk.vkDestroySampler(dd->device, dd->samp[i].s, NULL);
        for (uint32_t i = 0; i < dd->shader_slots.used; i++)
            if (dd->shader_slots.live[i]) {
                dd->vk.vkDestroyShaderModule(dd->device, dd->shader[i].module, NULL);
                free(dd->shader[i].words);   /* (step VC1: a pixel shader's code, gpu_vk_pipe.c) */
            }
        for (uint32_t i = 0; i < dd->qpool_slots.used; i++)
            if (dd->qpool_slots.live[i]) dd->vk.vkDestroyQueryPool(dd->device, dd->qpool[i].pool, NULL);
        for (uint32_t i = 0; i < dd->heap_slots.used; i++)
            if (dd->heap_slots.live[i]) dd->vk.vkFreeMemory(dd->device, dd->heap[i].memory, NULL);
        for (uint32_t i = 0; i < dd->tpool_slots.used; i++)
            if (dd->tpool_slots.live[i]) {
                dd->vk.vkDestroyDescriptorPool(dd->device, dd->tpool[i].pool, NULL);
                free(dd->tpool[i].sets);
                free(dd->tpool[i].set_at);
                free(dd->tpool[i].memo);
            }
        gpu_vk_pipe_release(dd);   /* pipelines and their layouts, before the set layouts */
        gpu_vk_desc_release(dd);
        for (uint32_t i = 0; i < dd->nslots; i++)
            if (dd->slot[i].pool) dd->vk.vkDestroyCommandPool(dd->device, dd->slot[i].pool, NULL);
        for (uint32_t i = 0; i < GPU_VK_INIT_RING; i++)
            if (dd->init_ring[i].pool) dd->vk.vkDestroyCommandPool(dd->device, dd->init_ring[i].pool, NULL);
        if (dd->oneshot_pool) dd->vk.vkDestroyCommandPool(dd->device, dd->oneshot_pool, NULL);
        if (dd->timeline) dd->vk.vkDestroySemaphore(dd->device, dd->timeline, NULL);
        dd->vk.vkDestroyDevice(dd->device, NULL);
    }
    if (dd->messenger) vkDestroyDebugUtilsMessengerEXT(dd->instance, dd->messenger, NULL);
    if (dd->instance) vkDestroyInstance(dd->instance, NULL);
    slots_fini(&dd->tex_slots);
    slots_fini(&dd->buf_slots);
    slots_fini(&dd->view_slots);
    slots_fini(&dd->samp_slots);
    slots_fini(&dd->shader_slots);
    slots_fini(&dd->layout_slots);
    slots_fini(&dd->qpool_slots);
    slots_fini(&dd->heap_slots);
    slots_fini(&dd->tpool_slots);
    slots_fini(&dd->swap_slots);
    free(dd->tex);
    free(dd->tex_layout);
    free(dd->buf_vk);
    free(dd->buf);
    free(dd->view);
    free(dd->samp);
    free(dd->samp_hash);
    free(dd->shader);
    free(dd->layout);
    free(dd->heap);
    free(dd->pso);
    free(dd->defer);
    free(dd->init);
    free(dd->marks);
    gpu_vk_mutex_fini(&dd->lock);
    gpu_vk_mutex_fini(&dd->qlock);
    gpu_vk_mutex_fini(&dd->pso_lock);
    free(dd);
}

static int device_init(gpu_vk_device* dd, const gpu_device_desc* desc)
{
    const uint32_t mask = desc->cap_mask | env_u32("RSX_VK_MASK", 0);
    if (!tables_init(dd)) return 0;
    if (!instance_make(dd, env_u32("RSX_VK_VALIDATION", 0) == 1)) return 0;
    if (!phys_pick(dd, desc)) return 0;
    vkGetPhysicalDeviceProperties(dd->phys, &dd->props);
    vkGetPhysicalDeviceMemoryProperties(dd->phys, &dd->mem);
    {
        VkPhysicalDeviceIDProperties id;
        VkPhysicalDeviceDriverProperties drv;
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp;
        VkPhysicalDeviceProperties2 p2;
        memset(&id, 0, sizeof id);
        memset(&drv, 0, sizeof drv);
        memset(&hp, 0, sizeof hp);
        memset(&p2, 0, sizeof p2);
        id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        hp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &id;
        id.pNext = &drv;
        vkGetPhysicalDeviceProperties2(dd->phys, &p2);
        if (id.deviceLUIDValid) {
            memcpy(dd->luid, id.deviceLUID, 8);
            dd->luid_valid = 1;
        }
        snprintf(dd->driver, sizeof dd->driver, "%.31s %.31s", drv.driverName, drv.driverInfo);
        if (!device_make(dd, desc, mask)) return 0;
        if (dd->has_host_import) {
            p2.pNext = &hp;
            vkGetPhysicalDeviceProperties2(dd->phys, &p2);
            dd->host_align = hp.minImportedHostPointerAlignment;
            if (!dd->host_align) dd->has_host_import = 0;
        }
    }
    dd->cb_range = dd->props.limits.maxUniformBufferRange < 65536u ? dd->props.limits.maxUniformBufferRange : 65536u;

    {   /* The timeline every fence is a point on. */
        VkSemaphoreTypeCreateInfo ti;
        VkSemaphoreCreateInfo si;
        memset(&ti, 0, sizeof ti);
        memset(&si, 0, sizeof si);
        ti.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        si.pNext = &ti;
        if (dd->vk.vkCreateSemaphore(dd->device, &si, NULL, &dd->timeline) != VK_SUCCESS) return 0;
    }

    /* A command pool and buffer per recorder (reset whole at each begin). */
    dd->nslots = desc->cmd_slots ? desc->cmd_slots : 2;
    if (dd->nslots > GPU_VK_MAX_CMD_SLOTS) dd->nslots = GPU_VK_MAX_CMD_SLOTS;
    for (uint32_t i = 0; i < dd->nslots; i++) {
        cmd_init(dd, &dd->slot[i]);
        if (!pool_make(dd, &dd->slot[i].pool, &dd->slot[i].cb, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT)) return 0;
    }
    for (uint32_t i = 0; i < GPU_VK_INIT_RING; i++)
        if (!pool_make(dd, &dd->init_ring[i].pool, &dd->init_ring[i].cb, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT))
            return 0;
    if (!pool_make(dd, &dd->oneshot_pool, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT)) return 0;
    {
        VkPipelineCacheCreateInfo pc;
        memset(&pc, 0, sizeof pc);
        pc.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        if (dd->vk.vkCreatePipelineCache(dd->device, &pc, NULL, &dd->pcache) != VK_SUCCESS) return 0;
    }
    if (dd->has_checkpoints) {
        dd->marks = (char*)calloc(GPU_VK_MARKS, GPU_VK_MARK_LEN);
        if (!dd->marks) dd->has_checkpoints = 0;
    }
    caps_make(dd, mask);
    return gpu_vk_desc_init(dd);
}

/* ---------------------------------------------------------------------------
 * Device functions
 * -------------------------------------------------------------------------*/

static void vkb_destroy(gpu_device* d)
{
    if (d) device_free(GPU_VK_DEV(d));
}

static void vkb_adapter(gpu_device* d, gpu_adapter_info* out)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%.127s", dd->props.deviceName);
    snprintf(out->driver, sizeof out->driver, "%s", dd->driver);
    out->vendor = dd->props.vendorID;
    out->device = dd->props.deviceID;
    out->driver_version = dd->props.driverVersion;
    for (uint32_t i = 0; i < dd->mem.memoryHeapCount; i++)
        if ((dd->mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && dd->mem.memoryHeaps[i].size > out->vram)
            out->vram = dd->mem.memoryHeaps[i].size;
    memcpy(out->luid, dd->luid, sizeof out->luid);
    memcpy(out->uuid, dd->props.pipelineCacheUUID, sizeof out->uuid);
}

/* The first VkResult that said the device was lost; Vulkan tells it only
 * through the calls' results. */
static uint32_t vkb_lost(gpu_device* d)
{
    return GPU_VK_DEV(d)->lost;
}

static uint32_t vkb_format_support(gpu_device* d, gpu_format format, uint32_t samples)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    const gpu_vk_format* f = gpu_vk_format_dev(dd, format, 0);
    if (!f) f = gpu_vk_format_dev(dd, format, GPU_USE_DS);
    if (!f) return 0;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(dd->phys, f->vk, &fp);
    const VkFormatFeatureFlags o = fp.optimalTilingFeatures;
    uint32_t r = 0;
    if (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) r |= GPU_FMT_SAMPLED;
    if (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) r |= GPU_FMT_FILTER;
    if (o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) r |= GPU_FMT_RT;
    if (o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) r |= GPU_FMT_BLEND;
    if (o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) r |= GPU_FMT_DS;
    if ((r & GPU_FMT_RT) && dd->has_surface &&
        (format == GPU_FORMAT_R8G8B8A8_UNORM || format == GPU_FORMAT_B8G8R8A8_UNORM ||
         format == GPU_FORMAT_R16G16B16A16_FLOAT || format == GPU_FORMAT_R10G10B10A2_UNORM))
        r |= GPU_FMT_DISPLAY;
    if (samples <= 1) {
        if (r & (GPU_FMT_RT | GPU_FMT_DS)) r |= GPU_FMT_MSAA;
    } else if (r & (GPU_FMT_RT | GPU_FMT_DS)) {
        VkImageFormatProperties ip;
        const VkImageUsageFlags u = (r & GPU_FMT_RT) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                                     : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (vkGetPhysicalDeviceImageFormatProperties(dd->phys, f->vk, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, u, 0,
                                                     &ip) == VK_SUCCESS && (ip.sampleCounts & samples))
            r |= GPU_FMT_MSAA;
    }
    return r;
}

static gpu_addr vkb_buffer_addr(gpu_device* d, gpu_buffer b)
{
    return gpu_fast_buffer_addr(d, b);
}

static void vkb_table_write_tex(gpu_device* d, gpu_table_pool pool, uint32_t index, const gpu_view_desc* srv)
{
    gpu_fast_table_write_tex(d, pool, index, srv);
}

static void vkb_table_write_sampler(gpu_device* d, gpu_table_pool pool, uint32_t index, gpu_sampler s)
{
    gpu_fast_table_write_sampler(d, pool, index, s);
}

/* The pool's tpool entry: pools live as long as the device. */
static gpu_table_writer vkb_table_writer_get(gpu_device* d, gpu_table_pool pool)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_table_writer w;
    w.v = gpu_vk_slot_valid(&dd->tpool_slots, pool.id)
              ? (uint64_t)(uintptr_t)&dd->tpool[GPU_HANDLE_INDEX(pool.id)] : 0;
    return w;
}

static void vkb_table_writer_tex(gpu_device* d, gpu_table_writer w, uint32_t index, const gpu_view_desc* srv)
{
    gpu_fast_table_writer_tex(d, w, index, srv);
}

static gpu_table vkb_table_ref(gpu_device* d, gpu_table_pool pool, uint32_t index)
{
    return gpu_fast_table_ref(d, pool, index);
}

/* The spv target is the compile service's (gpu_shader.c), later. */
static int vkb_compile_hook(gpu_device* d, const void* request, void* result)
{
    (void)d; (void)request; (void)result;
    return 0;
}

/* Occlusion queries, reset from the host when it can (a query must be reset
 * before each use: query_reset, outside a pass). GPU_QUERY_OCCLUSION counts
 * samples with GPU_CAP_OCCLUSION_PRECISE; without it, and for
 * GPU_QUERY_BINARY_OCCLUSION, a result is zero or not zero. */
static gpu_query_pool vkb_query_pool_create(gpu_device* d, uint32_t type, uint32_t count)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_query_pool r = {0};
    if ((type != GPU_QUERY_OCCLUSION && type != GPU_QUERY_BINARY_OCCLUSION) || !count) return r;
    VkQueryPoolCreateInfo qi;
    memset(&qi, 0, sizeof qi);
    qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qi.queryType = VK_QUERY_TYPE_OCCLUSION;
    qi.queryCount = count;
    VkQueryPool pool = VK_NULL_HANDLE;
    if (dd->vk.vkCreateQueryPool(dd->device, &qi, NULL, &pool) != VK_SUCCESS) return r;
    if (dd->has_host_reset) dd->vk.vkResetQueryPool(dd->device, pool, 0, count);
    gpu_vk_lock(&dd->lock);
    r.id = gpu_vk_slot_alloc(&dd->qpool_slots);
    if (r.id) {
        gpu_vk_qpool* const q = &dd->qpool[GPU_HANDLE_INDEX(r.id)];
        q->pool = pool;
        q->type = type;
        q->count = count;
        q->precise = (type == GPU_QUERY_OCCLUSION && dd->has_precise) ? VK_QUERY_CONTROL_PRECISE_BIT : 0u;
    }
    gpu_vk_unlock(&dd->lock);
    if (!r.id) dd->vk.vkDestroyQueryPool(dd->device, pool, NULL);
    return r;
}

/* ---------------------------------------------------------------------------
 * Recording
 * -------------------------------------------------------------------------*/

static void barrier_global(gpu_cmd* c, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
                           VkAccessFlags2 da)
{
    VkMemoryBarrier2 mb;
    VkDependencyInfo di;
    memset(&mb, 0, sizeof mb);
    memset(&di, 0, sizeof di);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = ss;
    mb.srcAccessMask = sa;
    mb.dstStageMask = ds;
    mb.dstAccessMask = da;
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    c->vk->vkCmdPipelineBarrier2(c->cb, &di);
}

/* Today's allocator and list Reset. Each recording starts with a barrier
 * from everything before (D3D12 orders command lists one after another and
 * lets buffers and textures in COMMON go back to it between them), and with
 * D3D12's starting state: stencil reference 0, blend factor 1, a triangle
 * list, primitive restart off. */
static gpu_cmd* vkb_cmd_begin(gpu_device* d, uint32_t slot)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    if (slot >= dd->nslots) return NULL;
    gpu_vk_defer_collect(dd, 0);
    gpu_cmd* const c = &dd->slot[slot];
    dd->vk.vkResetCommandPool(dd->device, c->pool, 0);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dd->vk.vkBeginCommandBuffer(c->cb, &bi) != VK_SUCCESS) return NULL;
    c->lay = NULL;
    memset(c->sets, 0, sizeof c->sets);
    c->have = 0;
    memset(c->dyn, 0, sizeof c->dyn);
    memset(&c->key, 0, sizeof c->key);
    c->set0_stale = 1;
    c->dirty = 0;
    memset(&c->pass, 0, sizeof c->pass);
    c->rendering = 0;
    c->npasses = 0;
    c->ro_depth = 0;
    c->dw = 1;
    c->pipe_dw = 0;
    c->restart = 0;
    barrier_global(c, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    {
        const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        dd->vk.vkCmdSetStencilReference(c->cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        dd->vk.vkCmdSetBlendConstants(c->cb, one);
        dd->vk.vkCmdSetPrimitiveTopology(c->cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        dd->vk.vkCmdSetPrimitiveRestartEnable(c->cb, VK_FALSE);
    }
    return c;
}

/* Ends the pass, and makes the recording's writes visible to the CPU (a
 * readback read after its fence). */
static void vkb_cmd_end(gpu_device* d, gpu_cmd* c)
{
    (void)d;
    if (c->rendering) gpu_vk__end(c);
    barrier_global(c, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                   VK_ACCESS_2_HOST_READ_BIT);
    c->vk->vkEndCommandBuffer(c->cb);
}

static void img_barrier(gpu_cmd* c, VkImageMemoryBarrier2* b, uint32_t i, VkImageLayout from, VkImageLayout to)
{
    const gpu_vk_tex* const t = &c->tex[i];
    memset(b, 0, sizeof *b);
    b->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b->srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b->srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b->dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b->dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b->oldLayout = from;
    b->newLayout = to;
    b->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b->image = t->image;
    b->subresourceRange.aspectMask = t->aspect;
    b->subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    b->subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
}

static void barriers_flush(gpu_cmd* c, const VkImageMemoryBarrier2* ib, uint32_t ni, const VkBufferMemoryBarrier2* bb,
                           uint32_t nb, const VkMemoryBarrier2* mb, uint32_t nm)
{
    if (!ni && !nb && !nm) return;
    VkDependencyInfo di;
    memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.memoryBarrierCount = nm;
    di.pMemoryBarriers = mb;
    di.bufferMemoryBarrierCount = nb;
    di.pBufferMemoryBarriers = bb;
    di.imageMemoryBarrierCount = ni;
    di.pImageMemoryBarriers = ib;
    c->vk->vkCmdPipelineBarrier2(c->cb, &di);
}

void gpu_vk_tex_touch(gpu_cmd* c, uint32_t i)
{
    if (c->tex_layout[i] != VK_IMAGE_LAYOUT_UNDEFINED) return;
    VkImageMemoryBarrier2 b;
    img_barrier(c, &b, i, VK_IMAGE_LAYOUT_UNDEFINED, c->tex[i].init_layout);
    c->tex_layout[i] = (uint32_t)b.newLayout;
    barriers_flush(c, &b, 1, NULL, 0, NULL, 0);
}

/* A subresource number's mip, layer and aspect (D3D12's planes: 0 depth,
 * 1 stencil). */
static void sub_of(const gpu_vk_tex* t, uint32_t sub, uint32_t* mip, uint32_t* layer, VkImageAspectFlags* aspect)
{
    const uint32_t per = (uint32_t)t->mips * t->layers;
    const uint32_t plane = sub / per, r = sub % per;
    *mip = r % t->mips;
    *layer = r / t->mips;
    if (t->aspect & VK_IMAGE_ASPECT_COLOR_BIT) *aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    else if (plane && (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) *aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
    else *aspect = t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
}

/* Between two passes of one recording: the first's target writes before
 * the second's loads and writes (D3D12 orders them; Vulkan orders only
 * within a pass). */
static void pass_order(gpu_cmd* c)
{
    if (c->npasses++ == 0) return;
    const VkPipelineStageFlags2 st = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    barrier_global(c, st, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, st,
                   VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

static void att_info(VkRenderingAttachmentInfo* a, VkImageView iv, VkImageLayout layout, int read_only)
{
    memset(a, 0, sizeof *a);
    a->sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    a->imageView = iv;
    a->imageLayout = layout;
    a->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a->storeOp = read_only ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE;
}

/* The pass's rendering: four colour slots always (the pipelines are made
 * for four, the unused ones UNDEFINED), each target in the layout it was
 * left in, over the smallest target. A depth view made read-only, or a depth
 * texture left in a read-only layout, is read only: nothing stored, depth
 * writes off. */
void gpu_vk_pass_open(gpu_cmd* c)
{
    const VkPhysicalDeviceLimits* const lim = &c->dd->props.limits;
    VkRenderingAttachmentInfo col[4], ds;
    VkRenderingInfo ri;
    uint32_t w = lim->maxFramebufferWidth, h = lim->maxFramebufferHeight;
    VkImageAspectFlags ds_aspect = 0;
    for (uint32_t i = 0; i < 4; i++) {
        att_info(&col[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, 0);
        if (i < c->pass.nrt && GPU_VALID(c->pass.rt[i])) {
            const gpu_vk_view* const v = &c->view[GPU_HANDLE_INDEX(c->pass.rt[i].id)];
            gpu_vk_tex_touch(c, v->tex);
            att_info(&col[i], v->iv, (VkImageLayout)c->tex_layout[v->tex], 0);
            if (v->width < w) w = v->width;
            if (v->height < h) h = v->height;
        }
    }
    c->ro_depth = 0;
    if (GPU_VALID(c->pass.ds)) {
        const gpu_vk_view* const v = &c->view[GPU_HANDLE_INDEX(c->pass.ds.id)];
        gpu_vk_tex_touch(c, v->tex);
        const VkImageLayout l = (VkImageLayout)c->tex_layout[v->tex];
        const int ro = (v->desc.flags & GPU_VIEW_READ_ONLY_DEPTH) || l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        att_info(&ds, v->iv, l, ro);
        ds_aspect = v->aspect;
        c->ro_depth = (uint32_t)ro;
        if (v->width < w) w = v->width;
        if (v->height < h) h = v->height;
    }
    pass_order(c);
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = w;
    ri.renderArea.extent.height = h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 4;
    ri.pColorAttachments = col;
    ri.pDepthAttachment = (ds_aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? &ds : NULL;
    ri.pStencilAttachment = (ds_aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? &ds : NULL;
    c->vk->vkCmdBeginRendering(c->cb, &ri);
    c->rendering = 1;
    c->area = ri.renderArea;
    if (c->pipe_dw) c->vk->vkCmdSetDepthWriteEnable(c->cb, (c->dw && !c->ro_depth) ? VK_TRUE : VK_FALSE);
}

/* Set 0 found again when its buffers changed, then every set bound in runs
 * of sets the recorder holds (a slot not bound yet leaves a gap). */
void gpu_vk_binds_slow(gpu_cmd* c)
{
    const gpu_vk_layout* const l = c->lay;
    c->dirty = 0;
    if (!l) return;
    if (l->ndyn && c->set0_stale) {
        const VkDescriptorSet s = gpu_vk_set0_get(c->dd, l, &c->key);
        c->sets[0] = s;
        if (s) c->have |= 1u;
        else c->have &= ~1u;
        c->set0_stale = 0;
    }
    uint32_t s = l->first;
    while (s < l->nsets) {
        if (!(c->have & (1u << s))) {
            s++;
            continue;
        }
        uint32_t e = s;
        while (e < l->nsets && (c->have & (1u << e))) e++;
        c->vk->vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, l->pl, s, e - s, &c->sets[s],
                                       s == 0 ? l->ndyn : 0u, c->dyn);
        s = e;
    }
}

/* A clear of a view the pass does not bind: in a rendering of its own. */
void gpu_vk_clear_unbound(gpu_cmd* c, gpu_view view, const VkClearValue* value, VkImageAspectFlags aspects,
                          const gpu_rect* rect)
{
    if (!GPU_VALID(view)) return;
    const gpu_vk_view* const v = &c->view[GPU_HANDLE_INDEX(view.id)];
    VkRenderingAttachmentInfo a;
    VkRenderingInfo ri;
    if (c->rendering) gpu_vk__end(c);
    gpu_vk_tex_touch(c, v->tex);
    pass_order(c);
    att_info(&a, v->iv, (VkImageLayout)c->tex_layout[v->tex], 0);
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = v->width;
    ri.renderArea.extent.height = v->height;
    ri.layerCount = 1;
    if (v->aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &a;
    } else {
        ri.pDepthAttachment = (v->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? &a : NULL;
        ri.pStencilAttachment = (v->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? &a : NULL;
    }
    c->vk->vkCmdBeginRendering(c->cb, &ri);
    c->area = ri.renderArea;
    gpu_vk__clear(c, 0, aspects, value, rect);
    c->vk->vkCmdEndRendering(c->cb);
}

static uint32_t mip_size(uint32_t v, uint32_t mip)
{
    v >>= mip;
    return v ? v : 1u;
}

void gpu_vk_rec_copy_texture(gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z,
                             gpu_texture src, uint32_t src_sub, const gpu_box* box)
{
    const uint32_t di = GPU_HANDLE_INDEX(dst.id), si = GPU_HANDLE_INDEX(src.id);
    const gpu_vk_tex* const dt = &c->tex[di];
    const gpu_vk_tex* const st = &c->tex[si];
    uint32_t smip, slayer, dmip, dlayer;
    VkImageAspectFlags sa, da;
    VkImageCopy2 r;
    VkCopyImageInfo2 ci;
    gpu_vk_tex_touch(c, di);
    gpu_vk_tex_touch(c, si);
    sub_of(st, src_sub, &smip, &slayer, &sa);
    sub_of(dt, dst_sub, &dmip, &dlayer, &da);
    memset(&r, 0, sizeof r);
    r.sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2;
    r.srcSubresource.aspectMask = sa;
    r.srcSubresource.mipLevel = smip;
    r.srcSubresource.baseArrayLayer = slayer;
    r.srcSubresource.layerCount = 1;
    r.dstSubresource.aspectMask = da;
    r.dstSubresource.mipLevel = dmip;
    r.dstSubresource.baseArrayLayer = dlayer;
    r.dstSubresource.layerCount = 1;
    if (box) {
        r.srcOffset.x = (int32_t)box->left;
        r.srcOffset.y = (int32_t)box->top;
        r.srcOffset.z = (int32_t)box->front;
        r.extent.width = box->right - box->left;
        r.extent.height = box->bottom - box->top;
        r.extent.depth = box->back > box->front ? box->back - box->front : 1u;
    } else {
        r.extent.width = mip_size(st->width, smip);
        r.extent.height = mip_size(st->height, smip);
        r.extent.depth = 1;
    }
    r.dstOffset.x = (int32_t)x;
    r.dstOffset.y = (int32_t)y;
    r.dstOffset.z = (int32_t)z;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2;
    ci.srcImage = st->image;
    ci.srcImageLayout = (VkImageLayout)c->tex_layout[si];
    ci.dstImage = dt->image;
    ci.dstImageLayout = (VkImageLayout)c->tex_layout[di];
    ci.regionCount = 1;
    ci.pRegions = &r;
    c->vk->vkCmdCopyImage2(c->cb, &ci);
}

/* CopyResource: every mip, every layer, every aspect. */
void gpu_vk_rec_copy_whole(gpu_cmd* c, gpu_texture dst, gpu_texture src)
{
    const uint32_t di = GPU_HANDLE_INDEX(dst.id), si = GPU_HANDLE_INDEX(src.id);
    const gpu_vk_tex* const st = &c->tex[si];
    VkImageCopy2 r[16];
    VkCopyImageInfo2 ci;
    uint32_t n = st->mips < 16 ? st->mips : 16u;
    gpu_vk_tex_touch(c, di);
    gpu_vk_tex_touch(c, si);
    memset(r, 0, sizeof r);
    for (uint32_t m = 0; m < n; m++) {
        r[m].sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2;
        r[m].srcSubresource.aspectMask = st->aspect;
        r[m].srcSubresource.mipLevel = m;
        r[m].srcSubresource.layerCount = st->layers;
        r[m].dstSubresource = r[m].srcSubresource;
        r[m].extent.width = mip_size(st->width, m);
        r[m].extent.height = mip_size(st->height, m);
        r[m].extent.depth = 1;
    }
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2;
    ci.srcImage = st->image;
    ci.srcImageLayout = (VkImageLayout)c->tex_layout[si];
    ci.dstImage = c->tex[di].image;
    ci.dstImageLayout = (VkImageLayout)c->tex_layout[di];
    ci.regionCount = n;
    ci.pRegions = r;
    c->vk->vkCmdCopyImage2(c->cb, &ci);
}

/* A buffer footprint to or from a subresource. The footprint's row pitch
 * becomes the row length in texels; its size (BC's rounded up to whole
 * blocks) is cut at the mip's edge, as Vulkan wants. */
void gpu_vk_rec_buf_tex(gpu_cmd* c, int to_tex, gpu_texture tx, uint32_t sub, uint32_t x, uint32_t y, uint32_t z,
                        gpu_buffer b, const gpu_footprint* fp, const gpu_box* box)
{
    const uint32_t ti = GPU_HANDLE_INDEX(tx.id);
    const gpu_vk_tex* const t = &c->tex[ti];
    uint32_t mip, layer;
    VkImageAspectFlags aspect;
    VkBufferImageCopy2 r;
    gpu_vk_tex_touch(c, ti);
    sub_of(t, sub, &mip, &layer, &aspect);
    const uint32_t bpb = aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? 1u : t->bpb;
    const uint32_t bw = t->bw;
    const uint32_t mw = mip_size(t->width, mip), mh = mip_size(t->height, mip);
    memset(&r, 0, sizeof r);
    r.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
    r.bufferOffset = fp->offset;
    r.bufferRowLength = fp->row_pitch / bpb * bw;
    r.bufferImageHeight = (fp->height + bw - 1) / bw * bw;
    r.imageSubresource.aspectMask = aspect;
    r.imageSubresource.mipLevel = mip;
    r.imageSubresource.baseArrayLayer = layer;
    r.imageSubresource.layerCount = 1;
    if (to_tex) {
        r.imageOffset.x = (int32_t)x;
        r.imageOffset.y = (int32_t)y;
        r.imageOffset.z = (int32_t)z;
        r.imageExtent.width = fp->width < mw - x ? fp->width : mw - x;
        r.imageExtent.height = fp->height < mh - y ? fp->height : mh - y;
        r.imageExtent.depth = fp->depth ? fp->depth : 1u;
        VkCopyBufferToImageInfo2 ci;
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2;
        ci.srcBuffer = c->buf[GPU_HANDLE_INDEX(b.id)];
        ci.dstImage = t->image;
        ci.dstImageLayout = (VkImageLayout)c->tex_layout[ti];
        ci.regionCount = 1;
        ci.pRegions = &r;
        c->vk->vkCmdCopyBufferToImage2(c->cb, &ci);
    } else {
        if (box) {
            r.imageOffset.x = (int32_t)box->left;
            r.imageOffset.y = (int32_t)box->top;
            r.imageOffset.z = (int32_t)box->front;
            r.imageExtent.width = box->right - box->left;
            r.imageExtent.height = box->bottom - box->top;
            r.imageExtent.depth = box->back > box->front ? box->back - box->front : 1u;
        } else {
            r.imageExtent.width = mw;
            r.imageExtent.height = mh;
            r.imageExtent.depth = 1;
        }
        VkCopyImageToBufferInfo2 ci;
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2;
        ci.srcImage = t->image;
        ci.srcImageLayout = (VkImageLayout)c->tex_layout[ti];
        ci.dstBuffer = c->buf[GPU_HANDLE_INDEX(b.id)];
        ci.regionCount = 1;
        ci.pRegions = &r;
        c->vk->vkCmdCopyImageToBuffer2(c->cb, &ci);
    }
}

void gpu_vk_rec_resolve(gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, gpu_texture src, uint32_t src_sub)
{
    const uint32_t di = GPU_HANDLE_INDEX(dst.id), si = GPU_HANDLE_INDEX(src.id);
    const gpu_vk_tex* const st = &c->tex[si];
    uint32_t smip, slayer, dmip, dlayer;
    VkImageAspectFlags sa, da;
    VkImageResolve2 r;
    VkResolveImageInfo2 ri;
    gpu_vk_tex_touch(c, di);
    gpu_vk_tex_touch(c, si);
    sub_of(st, src_sub, &smip, &slayer, &sa);
    sub_of(&c->tex[di], dst_sub, &dmip, &dlayer, &da);
    memset(&r, 0, sizeof r);
    r.sType = VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2;
    r.srcSubresource.aspectMask = sa;
    r.srcSubresource.mipLevel = smip;
    r.srcSubresource.baseArrayLayer = slayer;
    r.srcSubresource.layerCount = 1;
    r.dstSubresource.aspectMask = da;
    r.dstSubresource.mipLevel = dmip;
    r.dstSubresource.baseArrayLayer = dlayer;
    r.dstSubresource.layerCount = 1;
    r.extent.width = mip_size(st->width, smip);
    r.extent.height = mip_size(st->height, smip);
    r.extent.depth = 1;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2;
    ri.srcImage = st->image;
    ri.srcImageLayout = (VkImageLayout)c->tex_layout[si];
    ri.dstImage = c->tex[di].image;
    ri.dstImageLayout = (VkImageLayout)c->tex_layout[di];
    ri.regionCount = 1;
    ri.pRegions = &r;
    c->vk->vkCmdResolveImage2(c->cb, &ri);
}

void gpu_vk_rec_marker(gpu_cmd* c, const char* text, uint32_t len)
{
    gpu_vk_device* const dd = c->dd;
    if (len >= GPU_VK_MARK_LEN) len = GPU_VK_MARK_LEN - 1;
    if (dd->has_checkpoints) {
        const uint32_t k = (gpu_vk_add32(&dd->mark_next) - 1u) & (GPU_VK_MARKS - 1u);
        char* const m = dd->marks + (size_t)k * GPU_VK_MARK_LEN;
        memcpy(m, text, len);
        m[len] = 0;
        c->vk->vkCmdSetCheckpointNV(c->cb, m);
    } else if (dd->has_debug_utils && vkCmdInsertDebugUtilsLabelEXT) {
        char m[GPU_VK_MARK_LEN];
        VkDebugUtilsLabelEXT l;
        memcpy(m, text, len);
        m[len] = 0;
        memset(&l, 0, sizeof l);
        l.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        l.pLabelName = m;
        vkCmdInsertDebugUtilsLabelEXT(c->cb, &l);
    }
}

/* Many barriers in one call. A transition of one subresource moves it from
 * the layout its `before` state names (the whole texture first, when it has
 * never been used); an aliasing barrier starts its texture from UNDEFINED
 * (its contents are gone, as on D3D12), or with no texture orders all
 * memory. A depth-stencil texture's planes move together (Vulkan has one
 * layout for both). */
void gpu_vk_rec_barriers(gpu_cmd* c, uint32_t n, const gpu_barrier* list)
{
    VkImageMemoryBarrier2 ib[32];
    VkBufferMemoryBarrier2 bb[32];
    VkMemoryBarrier2 mb;
    uint32_t ni = 0, nb = 0, nm = 0;
    for (uint32_t k = 0; k < n; k++) {
        const gpu_barrier* const e = &list[k];
        if (ni == 32 || nb == 32) {
            barriers_flush(c, ib, ni, bb, nb, &mb, nm);
            ni = nb = nm = 0;
        }
        if (e->type == GPU_BARRIER_ALIASING) {
            if (GPU_VALID(e->texture)) {
                const uint32_t i = GPU_HANDLE_INDEX(e->texture.id);
                const VkImageLayout cur = (VkImageLayout)c->tex_layout[i];
                img_barrier(c, &ib[ni], i, VK_IMAGE_LAYOUT_UNDEFINED,
                            cur != VK_IMAGE_LAYOUT_UNDEFINED ? cur : c->tex[i].init_layout);
                c->tex_layout[i] = (uint32_t)ib[ni].newLayout;
                ni++;
            } else if (!nm) {
                memset(&mb, 0, sizeof mb);
                mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                nm = 1;
            }
        } else if (GPU_VALID(e->texture)) {
            const uint32_t i = GPU_HANDLE_INDEX(e->texture.id);
            const gpu_vk_tex* const t = &c->tex[i];
            VkImageMemoryBarrier2* const b = &ib[ni++];
            const VkImageLayout cur = (VkImageLayout)c->tex_layout[i];
            img_barrier(c, b, i, cur, gpu_vk_state_layout(e->after, t));
            gpu_vk_state_sync(e->before, 1, &b->srcStageMask, &b->srcAccessMask);
            gpu_vk_state_sync(e->after, 0, &b->dstStageMask, &b->dstAccessMask);
            if (e->sub != GPU_SUB_ALL && cur != VK_IMAGE_LAYOUT_UNDEFINED) {
                uint32_t mip, layer;
                VkImageAspectFlags a;
                sub_of(t, e->sub, &mip, &layer, &a);
                b->oldLayout = gpu_vk_state_layout(e->before, t);
                b->subresourceRange.baseMipLevel = mip;
                b->subresourceRange.levelCount = 1;
                b->subresourceRange.baseArrayLayer = layer;
                b->subresourceRange.layerCount = 1;
            }
            c->tex_layout[i] = (uint32_t)b->newLayout;
        } else if (GPU_VALID(e->buffer)) {
            VkBufferMemoryBarrier2* const b = &bb[nb++];
            memset(b, 0, sizeof *b);
            b->sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            gpu_vk_state_sync(e->before, 1, &b->srcStageMask, &b->srcAccessMask);
            gpu_vk_state_sync(e->after, 0, &b->dstStageMask, &b->dstAccessMask);
            b->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b->buffer = c->buf[GPU_HANDLE_INDEX(e->buffer.id)];
            b->size = VK_WHOLE_SIZE;
        }
    }
    barriers_flush(c, ib, ni, bb, nb, &mb, nm);
}

/* ---------------------------------------------------------------------------
 * Submitting, the timeline
 * -------------------------------------------------------------------------*/

/* Every submit signals the timeline (the new textures' first layouts and
 * deferred frees go by it); a fence is handed back only when asked. The new
 * textures' first layouts run first. */
static gpu_fence vkb_submit(gpu_device* d, gpu_cmd* const* cmds, uint32_t n, const gpu_submit_desc* how)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    VkCommandBufferSubmitInfo stack[33];
    VkCommandBufferSubmitInfo* cbi = n + 1 <= 33 ? stack
                                                 : (VkCommandBufferSubmitInfo*)malloc((n + 1) * sizeof *cbi);
    VkSemaphoreSubmitInfo wait[2], sig[3];
    uint32_t nwait = 0, nsig = 0, k = 0;
    if (!cbi) return 0;
    gpu_vk_lock(&dd->qlock);
    const uint64_t value = ++dd->timeline_value;
    const VkCommandBuffer init = gpu_vk_init_record(dd, value);
    memset(cbi, 0, (n + 1) * sizeof *cbi);
    if (init) {
        cbi[k].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cbi[k++].commandBuffer = init;
    }
    for (uint32_t i = 0; i < n; i++) {
        cbi[k].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cbi[k++].commandBuffer = cmds[i]->cb;
    }
    memset(wait, 0, sizeof wait);
    memset(sig, 0, sizeof sig);
    gpu_vk_swap_submit_sems(dd, how, wait, &nwait, sig, &nsig);
    sig[nsig].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    sig[nsig].semaphore = dd->timeline;
    sig[nsig].value = value;
    sig[nsig].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    nsig++;
    VkSubmitInfo2 si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.waitSemaphoreInfoCount = nwait;
    si.pWaitSemaphoreInfos = wait;
    si.commandBufferInfoCount = k;
    si.pCommandBufferInfos = cbi;
    si.signalSemaphoreInfoCount = nsig;
    si.pSignalSemaphoreInfos = sig;
    const VkResult r = dd->vk.vkQueueSubmit2(dd->queue, 1, &si, VK_NULL_HANDLE);
    gpu_vk_unlock(&dd->qlock);
    if (cbi != stack) free(cbi);
    if (r != VK_SUCCESS) gpu_vk_lost_note(dd, r);
    return how && (how->flags & GPU_SUBMIT_SIGNAL) ? value : 0;
}

/* A point after everything submitted so far: an empty submit that signals
 * it (a signal's scope is all work before it on the queue). */
static gpu_fence vkb_signal(gpu_device* d)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    VkSemaphoreSubmitInfo sig;
    VkSubmitInfo2 si;
    gpu_vk_lock(&dd->qlock);
    const uint64_t value = ++dd->timeline_value;
    memset(&sig, 0, sizeof sig);
    sig.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    sig.semaphore = dd->timeline;
    sig.value = value;
    sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &sig;
    const VkResult r = dd->vk.vkQueueSubmit2(dd->queue, 1, &si, VK_NULL_HANDLE);
    gpu_vk_unlock(&dd->qlock);
    if (r != VK_SUCCESS) gpu_vk_lost_note(dd, r);
    return value;
}

/* A lost device reads UINT64_MAX, as D3D12's fence does. Any thread. */
static uint64_t vkb_fence_completed(gpu_device* d)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    uint64_t v = 0;
    if (dd->lost) return UINT64_MAX;
    const VkResult r = dd->vk.vkGetSemaphoreCounterValue(dd->device, dd->timeline, &v);
    if (r != VK_SUCCESS) {
        gpu_vk_lost_note(dd, r);
        return UINT64_MAX;
    }
    return v;
}

static int vkb_fence_wait(gpu_device* d, gpu_fence f, uint32_t timeout_ms)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    if (vkb_fence_completed(d) >= f) return 0;
    VkSemaphoreWaitInfo wi;
    memset(&wi, 0, sizeof wi);
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wi.semaphoreCount = 1;
    wi.pSemaphores = &dd->timeline;
    wi.pValues = &f;
    const VkResult r = dd->vk.vkWaitSemaphores(dd->device, &wi, (uint64_t)timeout_ms * 1000000ull);
    if (r == VK_SUCCESS) return 0;
    if (r == VK_TIMEOUT) return dd->lost ? 0 : 1;
    gpu_vk_lost_note(dd, r);
    return 0;
}

static void vkb_wait_idle(gpu_device* d)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    const gpu_fence f = vkb_signal(d);
    while (vkb_fence_wait(d, f, 2000))
        if (dd->lost) break;
    gpu_vk_defer_collect(dd, 0);
}

VkCommandBuffer gpu_vk_oneshot_begin(gpu_vk_device* dd)
{
    VkCommandBufferAllocateInfo ai;
    VkCommandBufferBeginInfo bi;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = dd->oneshot_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    gpu_vk_lock(&dd->qlock);
    const VkResult r = dd->vk.vkAllocateCommandBuffers(dd->device, &ai, &cb);
    gpu_vk_unlock(&dd->qlock);
    if (r != VK_SUCCESS) return VK_NULL_HANDLE;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dd->vk.vkBeginCommandBuffer(cb, &bi);
    return cb;
}

int gpu_vk_oneshot_end(gpu_vk_device* dd, VkCommandBuffer cb)
{
    VkCommandBufferSubmitInfo cbi;
    VkSemaphoreSubmitInfo sig;
    VkSubmitInfo2 si;
    dd->vk.vkEndCommandBuffer(cb);
    memset(&cbi, 0, sizeof cbi);
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cbi.commandBuffer = cb;
    memset(&sig, 0, sizeof sig);
    sig.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    sig.semaphore = dd->timeline;
    sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &sig;
    gpu_vk_lock(&dd->qlock);
    sig.value = ++dd->timeline_value;
    VkResult r = dd->vk.vkQueueSubmit2(dd->queue, 1, &si, VK_NULL_HANDLE);
    gpu_vk_unlock(&dd->qlock);
    int ok = r == VK_SUCCESS && vkb_fence_wait(&dd->base, sig.value, 2000) == 0 && !dd->lost;
    if (r != VK_SUCCESS) gpu_vk_lost_note(dd, r);
    gpu_vk_lock(&dd->qlock);
    dd->vk.vkFreeCommandBuffers(dd->device, dd->oneshot_pool, 1, &cb);
    gpu_vk_unlock(&dd->qlock);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Diagnostics: the device fault report and the checkpoints
 * -------------------------------------------------------------------------*/

static void vkb_fault_report(gpu_device* d, const char* where)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    if (!dd->lost || dd->fault_done || gpu_vk_cas32(&dd->fault_done, 1, 0) != 0) return;
    fprintf(stderr, "[VK] device lost at %s (VkResult %d)\n", where ? where : "?", (int)(int32_t)dd->lost);
    if (dd->has_fault && dd->vk.vkGetDeviceFaultInfoEXT) {
        VkDeviceFaultCountsEXT fc;
        memset(&fc, 0, sizeof fc);
        fc.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
        if (dd->vk.vkGetDeviceFaultInfoEXT(dd->device, &fc, NULL) == VK_SUCCESS) {
            VkDeviceFaultAddressInfoEXT* ai =
                (VkDeviceFaultAddressInfoEXT*)calloc(fc.addressInfoCount + 1, sizeof *ai);
            VkDeviceFaultVendorInfoEXT* vi = (VkDeviceFaultVendorInfoEXT*)calloc(fc.vendorInfoCount + 1, sizeof *vi);
            VkDeviceFaultInfoEXT fi;
            memset(&fi, 0, sizeof fi);
            fi.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
            fi.pAddressInfos = ai;
            fi.pVendorInfos = vi;
            fc.vendorBinarySize = 0;
            if (ai && vi && dd->vk.vkGetDeviceFaultInfoEXT(dd->device, &fc, &fi) == VK_SUCCESS) {
                fprintf(stderr, "[VK] fault: %s\n", fi.description);
                for (uint32_t i = 0; i < fc.addressInfoCount; i++)
                    fprintf(stderr, "[VK]   address type %d at 0x%llx (+-0x%llx)\n", (int)ai[i].addressType,
                            (unsigned long long)ai[i].reportedAddress, (unsigned long long)ai[i].addressPrecision);
                for (uint32_t i = 0; i < fc.vendorInfoCount; i++)
                    fprintf(stderr, "[VK]   vendor: %s (code 0x%llx, data 0x%llx)\n", vi[i].description,
                            (unsigned long long)vi[i].vendorFaultCode, (unsigned long long)vi[i].vendorFaultData);
            }
            free(ai);
            free(vi);
        }
    }
    if (dd->has_checkpoints) {
        uint32_t n = 0;
        dd->vk.vkGetQueueCheckpointDataNV(dd->queue, &n, NULL);
        VkCheckpointDataNV* cd = (VkCheckpointDataNV*)calloc(n + 1, sizeof *cd);
        if (cd) {
            for (uint32_t i = 0; i < n; i++) cd[i].sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
            dd->vk.vkGetQueueCheckpointDataNV(dd->queue, &n, cd);
            for (uint32_t i = 0; i < n; i++)
                fprintf(stderr, "[VK]   checkpoint at stage 0x%x: %s\n", (unsigned)cd[i].stage,
                        cd[i].pCheckpointMarker ? (const char*)cd[i].pCheckpointMarker : "");
            free(cd);
        }
    }
    fflush(stderr);
}

/* The API's own objects, borrowed. */
static void* vkb_native(gpu_device* d, uint32_t kind, uint64_t handle)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    const uint32_t id = (uint32_t)handle, i = GPU_HANDLE_INDEX(id);
    switch (kind) {
    case GPU_NATIVE_DEVICE:   return (void*)dd->device;
    case GPU_NATIVE_QUEUE:    return (void*)dd->queue;
    case GPU_NATIVE_ADAPTER:  return (void*)dd->phys;
    case GPU_NATIVE_INSTANCE: return (void*)dd->instance;
    case GPU_NATIVE_CMD:      return handle ? (void*)((gpu_cmd*)(uintptr_t)handle)->cb : NULL;
    case GPU_NATIVE_BUFFER:
        return gpu_vk_slot_valid(&dd->buf_slots, id) ? (void*)(uintptr_t)dd->buf_vk[i] : NULL;
    case GPU_NATIVE_TEXTURE:
        return gpu_vk_slot_valid(&dd->tex_slots, id) ? (void*)(uintptr_t)dd->tex[i].image : NULL;
    case GPU_NATIVE_LAYOUT:
        return gpu_vk_slot_valid(&dd->layout_slots, id) ? (void*)(uintptr_t)dd->layout[i].pl : NULL;
    case GPU_NATIVE_SWAPCHAIN:
        return gpu_vk_slot_valid(&dd->swap_slots, id) ? (void*)(uintptr_t)dd->swap[i].sc : NULL;
    case GPU_NATIVE_PIPELINE: {   /* (gpu_pipeline.v is its index + 1) */
        VkPipeline p = VK_NULL_HANDLE;
        gpu_vk_lock(&dd->pso_lock);
        if (handle && handle <= dd->npso) p = dd->pso[handle - 1u].p;
        gpu_vk_unlock(&dd->pso_lock);
        return (void*)(uintptr_t)p;
    }
    default:                  return NULL;
    }
}

/* ---------------------------------------------------------------------------
 * The tables
 * -------------------------------------------------------------------------*/

#define vkb_buffer_create        gpu_vk_buffer_create
#define vkb_buffer_import_host   gpu_vk_buffer_import_host
#define vkb_buffer_destroy       gpu_vk_buffer_destroy
#define vkb_buffer_map           gpu_vk_buffer_map
#define vkb_buffer_unmap         gpu_vk_buffer_unmap
#define vkb_heap_create          gpu_vk_heap_create
#define vkb_texture_size         gpu_vk_texture_size
#define vkb_texture_footprints   gpu_vk_texture_footprints
#define vkb_texture_create       gpu_vk_texture_create
#define vkb_texture_destroy      gpu_vk_texture_destroy
#define vkb_view_get             gpu_vk_view_get
#define vkb_sampler_get          gpu_vk_sampler_get
#define vkb_table_pool_create    gpu_vk_table_pool_create
#define vkb_shader_create        gpu_vk_shader_create
#define vkb_shader_destroy       gpu_vk_shader_destroy
#define vkb_layout_create        gpu_vk_layout_create
#define vkb_pipeline_create      gpu_vk_pipeline_create
#define vkb_pipeline_cache_load  gpu_vk_pipeline_cache_load
#define vkb_pipeline_cache_save  gpu_vk_pipeline_cache_save
#define vkb_swap_create          gpu_vk_swap_create
#define vkb_swap_destroy         gpu_vk_swap_destroy
#define vkb_swap_resize          gpu_vk_swap_resize
#define vkb_swap_acquire         gpu_vk_swap_acquire
#define vkb_swap_image_count     gpu_vk_swap_image_count
#define vkb_swap_image           gpu_vk_swap_image
#define vkb_swap_present         gpu_vk_swap_present
#define vkb_swap_stats           gpu_vk_swap_stats
#define vkb_swap_hdr             gpu_vk_swap_hdr

#define VKB_VT_INIT(name, form, ret, params, args) vkb_##name,
static const gpu_vt k_vk_vt = { GPU_VT_FUNCS(VKB_VT_INIT) };
#undef VKB_VT_INIT

const gpu_cmd_ops gpu_vk_cmd_ops = GPU_CMD_OPS_TABLE;

gpu_device* gpu_vk_create(const gpu_device_desc* desc)
{
    gpu_device_desc none;
    if (!desc) {
        memset(&none, 0, sizeof none);
        desc = &none;
    }
    gpu_vk_device* const dd = (gpu_vk_device*)calloc(1, sizeof *dd);
    if (!dd) return NULL;
    gpu_vk_mutex_init(&dd->lock);
    gpu_vk_mutex_init(&dd->qlock);
    gpu_vk_mutex_init(&dd->pso_lock);
    dd->base.vt = &k_vk_vt;
    dd->flags = desc->flags;
    if (!device_init(dd, desc)) {
        device_free(dd);
        return NULL;
    }
    return &dd->base;
}

#endif /* PS3RECOMP_RSX_HAL && PS3RECOMP_RSX_HAL_VULKAN */
