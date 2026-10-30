/* ---------------------------------------------------------------------------
 * Render a frame (clear + present)
 * -----------------------------------------------------------------------*/

/* Write the mapped readback buffer (R8G8B8A8, row pitch = readback_pitch) out
 * as a 24-bit bottom-up BMP. Debug-only. */
/* Name prefix for the next readback dump: lets the same writer emit the
 * backbuffer and the screen-copy texture under different filenames. */
static const char* s_dump_name = NULL;

static void dump_backbuffer_bmp(void)
{
    if (!s_d3d.readback_buf) return;
    /* The GPU layer's readback buffer (step C2): its rows read. */
    void* mapped = gpu_buffer_map(s_rc_dev, s_rc_readback_buf, 0, (u64)s_d3d.readback_pitch * s_d3d.host_h);
    if (!mapped)
        return;

    /* CELLMARK_DUMP_CHANGED=1: write a frame only when it differs from the
     * last one written. A sampled hash (every 8th row, every 16th pixel) is
     * enough to tell a logo or a menu change from a re-presented frame, and a
     * long static screen then costs one file instead of hundreds. */
    { static int chg = -1; static unsigned long long last = 0;
      PS3_KNOB(chg, ps3_env_int("CELLMARK_DUMP_CHANGED", 0));
      if (chg && s_hotshot_left <= 0) {   /* an F10 burst keeps every present */
          unsigned long long hsh = 1469598103934665603ull;
          for (u32 y = 0; y < s_d3d.host_h; y += 8) {
              const u32* row = (const u32*)((unsigned char*)mapped + (u64)y * s_d3d.readback_pitch);
              for (u32 x = 0; x < s_d3d.host_w; x += 16) hsh = (hsh ^ row[x]) * 1099511628211ull;
          }
          if (hsh == last) {
              gpu_buffer_unmap(s_rc_dev, s_rc_readback_buf, 0, 0);
              return;
          }
          last = hsh;
      } }

    static int idx = 0;
    char path[512];
    const char* dir = getenv("CELLMARK_DUMP_DIR");   /* default: current dir */
    if (s_hotshot_left > 0 && !s_dump_name) {
        const char* hd = getenv("RSX_HOTKEY_DIR");
        if (!hd || !*hd) hd = "rsx_hotkey";   /* as F9 (cellGcmSys.c) */
        if (s_hotshot_name[0])
            snprintf(path, sizeof(path), "%s/%s_live_%02d_d%u.bmp", hd, s_hotshot_name,
                     s_hotshot_n++, s_dbg_last_draws);
        else
        snprintf(path, sizeof(path), "%s%sshot_%03d_%02d_d%u.bmp", hd ? hd : "", hd ? "/" : "",
                 s_hotshot_burst, s_hotshot_n++, s_dbg_last_draws);
        s_hotshot_left--;
    } else
    snprintf(path, sizeof(path), "%s%s%s_%03d.bmp",
             dir ? dir : "", dir ? "/" : "", s_dump_name ? s_dump_name : "frame", idx++);
    FILE* f = fopen(path, "wb");
    if (!f) printf("[D3D12] cannot write %s%c", path, 10);
    if (f) {
        u32 w = s_d3d.host_w, h = s_d3d.host_h;
        u32 padded = (w * 3 + 3) & ~3u;
        u32 imgsz  = padded * h;
        u32 filesz = 54 + imgsz;
        unsigned char hdr[54] = {0};
        hdr[0] = 'B'; hdr[1] = 'M';
        hdr[2]=filesz&0xFF; hdr[3]=(filesz>>8)&0xFF; hdr[4]=(filesz>>16)&0xFF; hdr[5]=(filesz>>24)&0xFF;
        hdr[10]=54; hdr[14]=40;
        hdr[18]=w&0xFF; hdr[19]=(w>>8)&0xFF; hdr[20]=(w>>16)&0xFF; hdr[21]=(w>>24)&0xFF;
        hdr[22]=h&0xFF; hdr[23]=(h>>8)&0xFF; hdr[24]=(h>>16)&0xFF; hdr[25]=(h>>24)&0xFF;
        hdr[26]=1; hdr[28]=24;
        hdr[34]=imgsz&0xFF; hdr[35]=(imgsz>>8)&0xFF; hdr[36]=(imgsz>>16)&0xFF; hdr[37]=(imgsz>>24)&0xFF;
        fwrite(hdr, 1, 54, f);
        unsigned char* row = (unsigned char*)malloc(padded);
        if (row) {
            memset(row, 0, padded);
            for (int y = (int)h - 1; y >= 0; y--) {   /* BMP is bottom-up */
                unsigned char* src = (unsigned char*)mapped + (u32)y * s_d3d.readback_pitch;
                for (u32 x = 0; x < w; x++) {
                    row[x*3+0] = src[x*4+2]; /* B */
                    row[x*3+1] = src[x*4+1]; /* G */
                    row[x*3+2] = src[x*4+0]; /* R */
                }
                fwrite(row, 1, padded, f);
            }
            free(row);
        }
        fclose(f);
        printf("[D3D12] dumped %s (%ux%u, %u draws)\n", path, s_d3d.host_w, s_d3d.host_h,
               s_dbg_last_draws);
    }
    gpu_buffer_unmap(s_rc_dev, s_rc_readback_buf, 0, 0);
}

