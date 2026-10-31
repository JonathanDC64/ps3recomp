/*
 * ps3recomp - the portable renderer's core: what the display says of HDR
 *
 * HDR output (rsxc_hdr.inc) needs to know whether the display the window is
 * on has HDR on, its peak brightness and the white level the system gives
 * SDR content there. That is the system's display code, not a backend's: on
 * Windows every backend reads it from DXGI's outputs by the window's monitor
 * (rsxc_display_win.c), Vulkan's included. Compiled once, for every instance
 * of the core. Plain C with <stdint.h> types only.
 */
#ifndef PS3RECOMP_RSXC_DISPLAY_H
#define PS3RECOMP_RSXC_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* What a display reports. */
typedef struct rsxc_display_hdr {
    int   hdr;     /* the system has HDR on there (an HDR10 colour space) */
    float peak;    /* its peak brightness, nits; 0 = not known */
    float white;   /* the SDR white level there ("SDR content brightness"), nits; 0 = not known */
} rsxc_display_hdr;

/* Asks the display the window is on now. No window, or nothing to ask, is
 * no HDR and nothing known. Render thread. */
void rsxc_display_hdr_query(void* window, rsxc_display_hdr* out);

/* 1 while the last query's answer still holds: one was made, the window is
 * on the same display, and the system's display setup has not changed since
 * (HDR switched, a display added). Render thread. */
int rsxc_display_hdr_current(void* window);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSXC_DISPLAY_H */
