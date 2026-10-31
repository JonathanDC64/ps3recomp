/*
 * ps3recomp - the portable renderer's core: the display's HDR, off Windows
 *
 * rsxc_display.h's queries where there is nothing to ask yet: SDL 2 says
 * nothing of a display's HDR, so every display is SDR and nothing is known
 * of it, and that answer always holds. A Linux HDR query (Wayland's colour
 * management, or Vulkan's HDR metadata) replaces this file (step VC2).
 * Compiled once.
 */
#if defined(PS3RECOMP_RSX_HAL) && !defined(_WIN32)

#include "rsxc_display.h"

void rsxc_display_hdr_query(void* window, rsxc_display_hdr* out)
{
    (void)window;
    out->hdr = 0;
    out->peak = out->white = 0.0f;
}

int rsxc_display_hdr_current(void* window)
{
    (void)window;
    return 1;
}

/* The two globals today's renderer defines and the core names as extern
 * (copy_manifest.txt), where today's renderer is not built: off Windows,
 * it is only with vkd3d. */
#if !defined(PS3RECOMP_RSX_D3D12_VKD3D)
char g_rsx_title_base[128] = "ps3recomp";
volatile long g_rsx_prof_toggle;   /* bumped per F11 press; boot_main starts/stops */
#endif

#endif /* PS3RECOMP_RSX_HAL && !_WIN32 */
