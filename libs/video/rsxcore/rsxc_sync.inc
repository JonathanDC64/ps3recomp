/* ---------------------------------------------------------------------------
 * Frame sync helpers
 * -----------------------------------------------------------------------*/

/* With the breadcrumbs knob (rc_be_breadcrumbs): on the first device
 * removal, the reason, the command history
 * around the last completed breadcrumb and the page-fault address, printed by
 * the GPU layer (gpu_fault_report: today's dred_list and dred_report, in the
 * D3D12 backend), once. */
static void dred_report(const char* where)
{
    if (s_rc_dev) gpu_fault_report(s_rc_dev, where);
}

static void wait_for_gpu(void)
{
    const gpu_fence want = gpu_signal(s_rc_dev);
    /* NEVER wait unbounded here: this runs on the vblank ticker, which also
     * drives the guest's vblank handlers, the FIFO drain, and the fence
     * publication. A device removal (TDR) leaves the D3D12 fence unsignaled
     * forever and an INFINITE wait silently froze the ENTIRE emulation --
     * observed on LBP as a boot that died the moment the 2048x2048
     * offscreen RT was created (last log line), every guest thread parked.
     * Degrade loudly instead: bounded waits + the removal reason, then
     * carry on (subsequent D3D calls fail visibly but the game keeps
     * running headless). */
    for (int tries = 0; tries < 3; tries++) {
        if (!gpu_fence_wait(s_rc_dev, want, 2000))
            return;
        const u32 rr = gpu_lost(s_rc_dev);
        printf("[D3D12] wait_for_gpu STUCK %ds: want %llu got %llu removed=0x%08lX\n",
               2 * (tries + 1),
               (unsigned long long)want,
               (unsigned long long)gpu_fence_completed(s_rc_dev),
               (unsigned long)rr);
        fflush(stdout);
        if (rr != 0 /* S_OK: device still alive, keep waiting a bit */)
            break;
    }
}

/* RSX_HANG_GPU_AT_FRAME=<n> (a test): the first indexed draw of the n-th
 * batch is drawn over and over, about 4G vertices in all, so the GPU takes
 * far longer than Windows' 2 s and the driver resets it, as it does for a
 * GPU that hangs; then the breadcrumbs show what they say of a real hang (a
 * removed device, as RSX_LOSE_DEVICE_AT_FRAME makes, has no command
 * history). It resets the adapter: run it on a GPU no display needs
 * (CELLMARK_IGPU=1). Read once. */
static UINT hang_test_instances(u32 index_count)
{
    static int at = -1, fired = 0;
    if (at < 0) {
        const char* e = getenv("RSX_HANG_GPU_AT_FRAME");
        at = e ? atoi(e) : 0;
    }
    if (at <= 0 || fired || s_rf_serial < (u32)at) return 1;
    fired = 1;
    const UINT n = 0xFFFFFFFFu / (index_count ? index_count : 1u);
    fprintf(stderr, "[D3D12] RSX_HANG_GPU_AT_FRAME: a draw %u times over in batch %u%c", n, s_rf_serial, 10);
    return n;
}

/* RSX_LOSE_DEVICE_AT_FRAME=<n> (a test): the device is removed on purpose
 * (the instance file's rc_be_lose_device: on D3D12, RemoveDevice) at
 * the n-th frame, as a driver reset would lose it, to see what follows: the
 * log line, the host's dialog. Read once. */
static void lose_device_test(void)
{
    static int at = -1, frames = 0;
    if (at < 0) {
        const char* e = getenv("RSX_LOSE_DEVICE_AT_FRAME");
        at = e ? atoi(e) : 0;
    }
    if (at <= 0 || ++frames != at) return;
    rc_be_lose_device(s_rc_dev, frames);
}

/* The end of a frame: a point on the timeline for its slot, the swap chain's
 * next buffer, and a wait (2 s at most) until the GPU is done with that
 * buffer's slot's use before this frame. When the buffer does not change
 * (RSX_PRESENT=0 presents nothing) that is the frame before, which
 * render_frame's opening wait_for_gpu has already seen through: the GPU runs
 * this frame while the next batch records, as in today's renderer (its slot's
 * fence value there is the opening wait's, already reached). The batch's
 * vertices go to the other parity half, and frame_cmd_begin waits for the
 * slot before its recorder is reset. */
static void move_to_next_frame(void)
{
    const u32 prev = s_d3d.frame_index;
    const gpu_fence mine = gpu_signal(s_rc_dev);

    s_d3d.frame_index = gpu_swap_acquire(s_rc_dev, s_rc_swap, NULL);

    lose_device_test();
    frame_slot_wait(s_d3d.frame_index);
    s_rc_frame_fence[prev] = mine;
}
