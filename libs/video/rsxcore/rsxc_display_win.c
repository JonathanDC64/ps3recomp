/*
 * ps3recomp - the portable renderer's core: the display's HDR, on Windows
 *
 * rsxc_display.h's queries (rsxc_hdr.inc's HDR output asks them between
 * frames): from DXGI's outputs, by the monitor the window is on, and the SDR
 * white level from the display configuration, as today's renderer reads them
 * (rsx_d3d12_backend.c's hdr_sdr_white and hdr_display_query, moved here from
 * rsxc_hdr.inc by step C8b). Every backend on Windows asks this, not only
 * D3D12's: DXGI says it for any adapter. Compiled once.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "rsxc_display.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

/* DISPLAYCONFIG_SDR_WHITE_LEVEL, declared here: wingdi.h has it only for a
 * newer target Windows version than this file asks for. */
typedef struct { DISPLAYCONFIG_DEVICE_INFO_HEADER header; ULONG level; } HdrSdrWhite;

/* Windows' SDR white level ("SDR content brightness") on the display with
 * this GDI device name, in nits; 0 when not known. */
static float hdr_sdr_white(const WCHAR* gdi)
{
    UINT32 np = 0, nm = 0;
    float nits = 0.0f;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np) return 0.0f;
    DISPLAYCONFIG_PATH_INFO* pi = (DISPLAYCONFIG_PATH_INFO*)calloc(np, sizeof *pi);
    DISPLAYCONFIG_MODE_INFO* mi = (DISPLAYCONFIG_MODE_INFO*)calloc(nm ? nm : 1, sizeof *mi);
    if (pi && mi && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, pi, &nm, mi, NULL) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < np && nits == 0.0f; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
            memset(&sn, 0, sizeof sn);
            sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sn.header.size = sizeof sn;
            sn.header.adapterId = pi[i].sourceInfo.adapterId;
            sn.header.id = pi[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || wcscmp(sn.viewGdiDeviceName, gdi))
                continue;
            HdrSdrWhite wl;
            memset(&wl, 0, sizeof wl);
            wl.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)11;   /* ..._GET_SDR_WHITE_LEVEL */
            wl.header.size = sizeof wl;
            wl.header.adapterId = pi[i].targetInfo.adapterId;
            wl.header.id = pi[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS) nits = (float)wl.level / 1000.0f * 80.0f;
        }
    }
    free(pi);
    free(mi);
    return nits;
}

/* The last query's factory and the window's display then. A new factory
 * each time: one keeps describing the outputs as they were when it was made
 * (IsCurrent says when that changed). */
static IDXGIFactory1* s_hdr_factory;
static HMONITOR s_hdr_mon;
static int s_hdr_no_window;   /* the last query had no window to ask about */

void rsxc_display_hdr_query(void* window, rsxc_display_hdr* out)
{
    out->hdr = 0;
    out->peak = out->white = 0.0f;
    if (s_hdr_factory) s_hdr_factory->lpVtbl->Release(s_hdr_factory);
    s_hdr_factory = NULL;
    s_hdr_no_window = !window;
    if (!window) {   /* (a headless backend's: no display) */
        s_hdr_mon = NULL;
        return;
    }
    s_hdr_mon = MonitorFromWindow((HWND)window, MONITOR_DEFAULTTONEAREST);
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&s_hdr_factory))) {
        s_hdr_factory = NULL;
        return;
    }
    IDXGIAdapter1* a = NULL;
    for (UINT i = 0; s_hdr_factory->lpVtbl->EnumAdapters1(s_hdr_factory, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        IDXGIOutput* o = NULL;
        for (UINT k = 0; a->lpVtbl->EnumOutputs(a, k, &o) != DXGI_ERROR_NOT_FOUND; k++) {
            DXGI_OUTPUT_DESC d;
            IDXGIOutput6* o6 = NULL;
            if (SUCCEEDED(o->lpVtbl->GetDesc(o, &d)) && d.Monitor == s_hdr_mon &&
                SUCCEEDED(o->lpVtbl->QueryInterface(o, &IID_IDXGIOutput6, (void**)&o6))) {
                DXGI_OUTPUT_DESC1 d1;
                if (SUCCEEDED(o6->lpVtbl->GetDesc1(o6, &d1))) {
                    out->hdr = d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    out->peak = d1.MaxLuminance;
                    out->white = hdr_sdr_white(d1.DeviceName);
                }
                o6->lpVtbl->Release(o6);
            }
            o->lpVtbl->Release(o);
        }
        a->lpVtbl->Release(a);
    }
}

int rsxc_display_hdr_current(void* window)
{
    if (!window) return s_hdr_no_window;   /* nothing to ask again until there is a window */
    return s_hdr_factory && MonitorFromWindow((HWND)window, MONITOR_DEFAULTTONEAREST) == s_hdr_mon &&
           s_hdr_factory->lpVtbl->IsCurrent(s_hdr_factory);
}

#endif /* PS3RECOMP_RSX_HAL && _WIN32 */
