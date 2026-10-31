/*
 * ps3recomp - the portable renderer's core off Windows: what it names of
 * Windows beyond runtime/platform/win32_compat.h
 *
 * rsxc_vk.c includes this after win32_compat.h on Linux (step VC2).
 */
#ifndef PS3RECOMP_RSXC_POSIX_H
#define PS3RECOMP_RSXC_POSIX_H

#ifndef _WIN32
#include <stdio.h>

/* The window is the core's SDL one: an SDL_Window*, as rsx_vkd3d_host.h has it. */
typedef void* HWND;

/* The core keeps a result as an HRESULT: the hooks' codes (a VkResult here) and
 * its own few, with the values Windows gives them. */
typedef int32_t HRESULT;
#define S_OK            ((HRESULT)0)
#define S_FALSE         ((HRESULT)1)
#define E_FAIL          ((HRESULT)0x80004005)
#define E_OUTOFMEMORY   ((HRESULT)0x8007000E)
#define E_INVALIDARG    ((HRESULT)0x80070057)
#define SUCCEEDED(hr)   ((HRESULT)(hr) >= 0)
#define FAILED(hr)      ((HRESULT)(hr) < 0)

/* __declspec(thread): the core's thread-local buffers. */
#define __declspec(a)          RSXC_DECLSPEC_##a
#define RSXC_DECLSPEC_thread   __thread

/* GetFileAttributesExA for the size only (pso_file_size asks nothing else),
 * through stdio: win32_compat.h keeps out of <sys/stat.h>. */
typedef struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;
enum { GetFileExInfoStandard = 0 };
static inline BOOL GetFileAttributesExA(const char* path, int level, WIN32_FILE_ATTRIBUTE_DATA* fa)
{
    FILE* f = fopen(path, "rb");
    long long n;
    (void)level;
    if (!f) return 0;
    n = fseeko(f, 0, SEEK_END) == 0 ? (long long)ftello(f) : -1;
    fclose(f);
    if (n < 0) return 0;
    fa->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    fa->nFileSizeHigh = (DWORD)((unsigned long long)n >> 32);
    fa->nFileSizeLow  = (DWORD)n;
    return 1;
}

#endif /* !_WIN32 */

#endif /* PS3RECOMP_RSXC_POSIX_H */
