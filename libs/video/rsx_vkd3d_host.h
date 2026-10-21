/*
 * ps3recomp - what the D3D12 backend takes from Windows, on a host without it
 *
 * rsx_d3d12_backend.c is written against D3D12, DXGI and Win32. Built with
 * PS3RECOMP_RSX_D3D12_VKD3D off Windows, it gets D3D12 and D3DCompile from
 * vkd3d (Wine's D3D12 on Vulkan), the Win32 types and calls from
 * runtime/platform/win32_compat.h, and the few DXGI interfaces it uses from
 * rsx_vkd3d_dxgi.c, which presents through a Vulkan swap chain to the SDL
 * window the backend opens. A window handle (HWND) is an SDL_Window* here.
 *
 * Only the backend and rsx_vkd3d_dxgi.c include this. The backend defines
 * RSX_VKD3D_DEFINE_GUIDS first, so the interface GUIDs are defined once.
 */
#ifndef RSX_VKD3D_HOST_H
#define RSX_VKD3D_HOST_H

#if !defined(_WIN32) && defined(PS3RECOMP_RSX_D3D12_VKD3D)

#ifdef RSX_VKD3D_DEFINE_GUIDS
#  define INITGUID
#endif
#include <vkd3d.h>
#include <vkd3d_utils.h>
#include <vkd3d_d3d12sdklayers.h>
#include <vkd3d_d3dcompiler.h>   /* D3DCOMPILE_* */

/* vkd3d declares its entry points and COM methods with the Windows calling
 * convention (ms_abi on x86-64) through WINAPI and STDMETHODCALLTYPE. Its
 * prototypes keep that; from here on WINAPI is nothing again, because
 * win32_compat.h's thread functions are plain C functions and the backend's
 * thread procs, spelt WINAPI, are handed to them. */
#undef WINAPI
#define WINAPI
#include "../../runtime/platform/win32_compat.h"
#include "../../runtime/platform/win32_backtrace.h"   /* GetModuleFileNameA */
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* HWND;

/* The MSVC spellings the backend uses. */
#define _snwprintf      swprintf
#define _byteswap_ulong __builtin_bswap32
#define __forceinline   inline __attribute__((always_inline))
#define __declspec(x)   RSX__DECLSPEC_##x
#define RSX__DECLSPEC_align(n) __attribute__((aligned(n)))
static inline unsigned char _BitScanReverse(unsigned long* index, unsigned long mask)
{
    if (!mask) return 0;
    *index = (unsigned long)(sizeof mask * 8 - 1) - (unsigned long)__builtin_clzl(mask);
    return 1;
}

/* The device comes from rsx_vkd3d_dxgi.c rather than vkd3d-utils, whose own
 * fences signal vkd3d-utils events: the backend waits on win32_compat events,
 * and the device needs the window's Vulkan surface extensions. The adapter is
 * not used (VKD3D_VULKAN_DEVICE picks a GPU). */
#undef D3D12CreateDevice
#define D3D12CreateDevice rsx_vkd3d_create_device
HRESULT rsx_vkd3d_create_device(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid, void** device);

/* ---------------------------------------------------------------------------
 * The DXGI the backend uses
 * -----------------------------------------------------------------------*/

typedef UINT DXGI_USAGE;
#define DXGI_USAGE_RENDER_TARGET_OUTPUT       0x20u
#define DXGI_PRESENT_ALLOW_TEARING            0x200u
#define DXGI_MWA_NO_ALT_ENTER                 0x2u
#define DXGI_STATUS_OCCLUDED                  _HRESULT_TYPEDEF_(0x087A0001)
#define DXGI_ERROR_FRAME_STATISTICS_DISJOINT  _HRESULT_TYPEDEF_(0x887A000B)

typedef enum DXGI_SWAP_EFFECT {
    DXGI_SWAP_EFFECT_DISCARD = 0,
    DXGI_SWAP_EFFECT_SEQUENTIAL = 1,
    DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL = 3,
    DXGI_SWAP_EFFECT_FLIP_DISCARD = 4,
} DXGI_SWAP_EFFECT;
typedef enum DXGI_SCALING {
    DXGI_SCALING_STRETCH = 0,
    DXGI_SCALING_NONE = 1,
    DXGI_SCALING_ASPECT_RATIO_STRETCH = 2,
} DXGI_SCALING;
typedef enum DXGI_ALPHA_MODE {
    DXGI_ALPHA_MODE_UNSPECIFIED = 0,
    DXGI_ALPHA_MODE_PREMULTIPLIED = 1,
    DXGI_ALPHA_MODE_STRAIGHT = 2,
    DXGI_ALPHA_MODE_IGNORE = 3,
} DXGI_ALPHA_MODE;
enum { DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING = 2048 };
typedef enum DXGI_FEATURE { DXGI_FEATURE_PRESENT_ALLOW_TEARING = 0 } DXGI_FEATURE;
typedef enum DXGI_GPU_PREFERENCE {
    DXGI_GPU_PREFERENCE_UNSPECIFIED = 0,
    DXGI_GPU_PREFERENCE_MINIMUM_POWER = 1,
    DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE = 2,
} DXGI_GPU_PREFERENCE;
enum { DXGI_ADAPTER_FLAG_NONE = 0, DXGI_ADAPTER_FLAG_REMOTE = 1, DXGI_ADAPTER_FLAG_SOFTWARE = 2 };

typedef struct DXGI_SWAP_CHAIN_DESC1 {
    UINT Width;
    UINT Height;
    DXGI_FORMAT Format;
    BOOL Stereo;
    DXGI_SAMPLE_DESC SampleDesc;
    DXGI_USAGE BufferUsage;
    UINT BufferCount;
    DXGI_SCALING Scaling;
    DXGI_SWAP_EFFECT SwapEffect;
    DXGI_ALPHA_MODE AlphaMode;
    UINT Flags;
} DXGI_SWAP_CHAIN_DESC1;

typedef struct DXGI_FRAME_STATISTICS {
    UINT PresentCount;
    UINT PresentRefreshCount;
    UINT SyncRefreshCount;
    LARGE_INTEGER SyncQPCTime;
    LARGE_INTEGER SyncGPUTime;
} DXGI_FRAME_STATISTICS;

typedef struct DXGI_ADAPTER_DESC1 {
    WCHAR Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    LUID AdapterLuid;
    UINT Flags;
} DXGI_ADAPTER_DESC1;

/* One interface per object, carrying every method the backend calls on any
 * of its versions (IDXGIFactory4/5/6 are the factory, IDXGISwapChain1/3 the
 * swap chain): nothing outside this backend sees these vtables. */
typedef struct IDXGIAdapter1 IDXGIAdapter1;
typedef struct IDXGIAdapter1Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDXGIAdapter1* This, REFIID iid, void** out);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDXGIAdapter1* This);
    ULONG   (STDMETHODCALLTYPE *Release)(IDXGIAdapter1* This);
    HRESULT (STDMETHODCALLTYPE *GetDesc1)(IDXGIAdapter1* This, DXGI_ADAPTER_DESC1* desc);
} IDXGIAdapter1Vtbl;
struct IDXGIAdapter1 { const IDXGIAdapter1Vtbl* lpVtbl; };

typedef struct IDXGISwapChain3 IDXGISwapChain3;
typedef IDXGISwapChain3 IDXGISwapChain1;
typedef struct IDXGISwapChain3Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDXGISwapChain3* This, REFIID iid, void** out);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDXGISwapChain3* This);
    ULONG   (STDMETHODCALLTYPE *Release)(IDXGISwapChain3* This);
    HRESULT (STDMETHODCALLTYPE *Present)(IDXGISwapChain3* This, UINT sync_interval, UINT flags);
    HRESULT (STDMETHODCALLTYPE *GetBuffer)(IDXGISwapChain3* This, UINT index, REFIID iid, void** out);
    HRESULT (STDMETHODCALLTYPE *ResizeBuffers)(IDXGISwapChain3* This, UINT count, UINT width, UINT height,
                                               DXGI_FORMAT format, UINT flags);
    HRESULT (STDMETHODCALLTYPE *GetFrameStatistics)(IDXGISwapChain3* This, DXGI_FRAME_STATISTICS* stats);
    HRESULT (STDMETHODCALLTYPE *GetLastPresentCount)(IDXGISwapChain3* This, UINT* count);
    UINT    (STDMETHODCALLTYPE *GetCurrentBackBufferIndex)(IDXGISwapChain3* This);
} IDXGISwapChain3Vtbl;
struct IDXGISwapChain3 { const IDXGISwapChain3Vtbl* lpVtbl; };

typedef struct IDXGIFactory6 IDXGIFactory6;
typedef IDXGIFactory6 IDXGIFactory4;
typedef IDXGIFactory6 IDXGIFactory5;
typedef struct IDXGIFactory6Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDXGIFactory6* This, REFIID iid, void** out);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDXGIFactory6* This);
    ULONG   (STDMETHODCALLTYPE *Release)(IDXGIFactory6* This);
    HRESULT (STDMETHODCALLTYPE *MakeWindowAssociation)(IDXGIFactory6* This, HWND window, UINT flags);
    HRESULT (STDMETHODCALLTYPE *CreateSwapChainForHwnd)(IDXGIFactory6* This, IUnknown* queue, HWND window,
                                                        const DXGI_SWAP_CHAIN_DESC1* desc,
                                                        const void* fullscreen_desc, void* restrict_to_output,
                                                        IDXGISwapChain1** swap_chain);
    HRESULT (STDMETHODCALLTYPE *CheckFeatureSupport)(IDXGIFactory6* This, DXGI_FEATURE feature, void* data,
                                                     UINT size);
    HRESULT (STDMETHODCALLTYPE *EnumAdapterByGpuPreference)(IDXGIFactory6* This, UINT index,
                                                            DXGI_GPU_PREFERENCE preference, REFIID iid,
                                                            void** adapter);
} IDXGIFactory6Vtbl;
struct IDXGIFactory6 { const IDXGIFactory6Vtbl* lpVtbl; };

extern const GUID IID_IDXGIFactory4, IID_IDXGIFactory5, IID_IDXGIFactory6;
extern const GUID IID_IDXGIAdapter1, IID_IDXGISwapChain1, IID_IDXGISwapChain3;

HRESULT CreateDXGIFactory1(REFIID iid, void** factory);

#ifdef __cplusplus
}
#endif

#endif /* !_WIN32 && PS3RECOMP_RSX_D3D12_VKD3D */
#endif /* RSX_VKD3D_HOST_H */
