/* ---------------------------------------------------------------------------
 * Frame sync helpers
 * -----------------------------------------------------------------------*/

/* One command list of DRED's history: its op count, and for a list the GPU
 * had not finished, the ops around the first one not done, what came before
 * it, and (DRED 1.2) the text of the markers dred_mark put before each draw.
 * ctx: D3D12_DRED_BREADCRUMB_CONTEXT[nctx], or none. */
static void dred_list(const void* list, u32 count, const UINT* lastp,
                      const D3D12_AUTO_BREADCRUMB_OP* hist, const void* ctx, u32 nctx)
{
    static const char* nm[] = { "SETMARKER","BEGINEVENT","ENDEVENT","DRAWINSTANCED",
        "DRAWINDEXEDINSTANCED","EXECUTEINDIRECT","DISPATCH","COPYBUFFERREGION",
        "COPYTEXTUREREGION","COPYRESOURCE","COPYTILES","RESOLVESUBRESOURCE",
        "CLEARRENDERTARGETVIEW","CLEARUNORDEREDACCESSVIEW","CLEARDEPTHSTENCILVIEW",
        "RESOURCEBARRIER" };
    const u32 last = lastp ? *lastp : 0;
    fprintf(stderr, "[DRED] list %p: %u ops, last completed %u, %u markers%c",
            list, count, last, nctx, 10);
    if (last >= count || !hist) return;   /* list finished */
    /* The text of op k's marker, if it has one. */
#ifdef _WIN32
    const D3D12_DRED_BREADCRUMB_CONTEXT* cx = (const D3D12_DRED_BREADCRUMB_CONTEXT*)ctx;
#define DRED_TEXT_AT(k, out) do { out = NULL; \
        for (u32 _i = 0; cx && _i < nctx; _i++) \
            if (cx[_i].BreadcrumbIndex == (k)) { out = cx[_i].pContextString; break; } } while (0)
#else
    (void)ctx;
#define DRED_TEXT_AT(k, out) (out = NULL)
#endif
    u32 draws = 0, clears = 0, copies = 0, barriers = 0;
    for (u32 k = 0; k < last; k++) {
        const u32 op = (u32)hist[k];
        if (op == 3 || op == 4) draws++;
        else if (op >= 12 && op <= 14) clears++;
        else if (op >= 7 && op <= 11) copies++;
        else if (op == 15) barriers++;
    }
    fprintf(stderr, "[DRED]   done before it: %u draws, %u clears, %u copies, %u barriers%c",
            draws, clears, copies, barriers, 10);
    /* The GPU works on several draws at once: the first one not done is
     * where it stopped, or a draw or two after it. */
    const u32 a = last > 16 ? last - 16 : 0, b = last + 24 < count ? last + 24 : count;
    for (u32 k = a; k < b; k++) {
        const u32 op = (u32)hist[k];
        const wchar_t* t;
        DRED_TEXT_AT(k, t);
        if (t)
            fprintf(stderr, "[DRED]   %s%5u %s: %ls%c", k == last ? ">>" : "  ", k,
                    op < sizeof nm / sizeof nm[0] ? nm[op] : "op", t, 10);
        else
            fprintf(stderr, "[DRED]   %s%5u %s (%u)%c", k == last ? ">>" : "  ", k,
                    op < sizeof nm / sizeof nm[0] ? nm[op] : "op", op, 10);
    }
    for (u32 k = last + 1; k-- > 0; ) {   /* the marker of the draw not done */
        const wchar_t* t;
        DRED_TEXT_AT(k, t);
        if (t) { fprintf(stderr, "[DRED] the GPU stopped at or just after: %ls%c", t, 10); break; }
    }
#undef DRED_TEXT_AT
}

/* D3D12_DRED=1: on the first device removal, print the reason, the command
 * history around the last completed breadcrumb, and the page-fault address. */
static void dred_report(const char* where)
{
    static int done = 0;
    if (done || !getenv("D3D12_DRED") || !s_d3d.device) return;
    HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
    if (rr == S_OK) return;
    done = 1;
    fprintf(stderr, "[DRED] device removed at %s: reason 0x%08lX%c", where, (long)rr, 10);
    ID3D12DeviceRemovedExtendedData* d = NULL;
    if (FAILED(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device,
            &IID_ID3D12DeviceRemovedExtendedData, (void**)&d)) || !d) {
        fprintf(stderr, "[DRED] no DRED interface%c", 10); return;
    }
    int listed = 0;
#ifdef _WIN32
    {   /* DRED 1.2 has the markers' text too. */
        ID3D12DeviceRemovedExtendedData1* d1 = NULL;
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bo1 = {0};
        if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device,
                &IID_ID3D12DeviceRemovedExtendedData1, (void**)&d1)) && d1) {
            if (SUCCEEDED(d1->lpVtbl->GetAutoBreadcrumbsOutput1(d1, &bo1))) {
                listed = 1;
                fprintf(stderr, "[DRED] version 1.2 output%c", 10);
                for (const D3D12_AUTO_BREADCRUMB_NODE1* n = bo1.pHeadAutoBreadcrumbNode; n; n = n->pNext)
                    dred_list((const void*)n->pCommandList, n->BreadcrumbCount, n->pLastBreadcrumbValue,
                              n->pCommandHistory, n->pBreadcrumbContexts, n->BreadcrumbContextsCount);
            }
            d1->lpVtbl->Release(d1);
        }
    }
#endif
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bo = {0};
    if (!listed && SUCCEEDED(d->lpVtbl->GetAutoBreadcrumbsOutput(d, &bo))) {
        for (const D3D12_AUTO_BREADCRUMB_NODE* n = bo.pHeadAutoBreadcrumbNode; n; n = n->pNext)
            dred_list((const void*)n->pCommandList, n->BreadcrumbCount, n->pLastBreadcrumbValue,
                      n->pCommandHistory, NULL, 0);
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT po = {0};
    if (SUCCEEDED(d->lpVtbl->GetPageFaultAllocationOutput(d, &po))) {
        if (po.PageFaultVA)
            fprintf(stderr, "[DRED] page fault VA 0x%llX%c", (unsigned long long)po.PageFaultVA, 10);
        else
            fprintf(stderr, "[DRED] page fault VA 0x0 (none recorded)%c", 10);
        for (const D3D12_DRED_ALLOCATION_NODE* a = po.pHeadExistingAllocationNode; a; a = a->pNext)
            fprintf(stderr, "[DRED]   existing alloc type %d %s%c", (int)a->AllocationType,
                    a->ObjectNameA ? a->ObjectNameA : "", 10);
        for (const D3D12_DRED_ALLOCATION_NODE* a = po.pHeadRecentFreedAllocationNode; a; a = a->pNext)
            fprintf(stderr, "[DRED]   freed alloc type %d %s%c", (int)a->AllocationType,
                    a->ObjectNameA ? a->ObjectNameA : "", 10);
    }
    d->lpVtbl->Release(d);
    fflush(stderr);
}

static void wait_for_gpu(void)
{
    u32 fi = s_d3d.frame_index;
    s_d3d.fence_values[fi]++;
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.fence, s_d3d.fence_values[fi]);

    if (s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) < s_d3d.fence_values[fi]) {
        s_d3d.fence->lpVtbl->SetEventOnCompletion(
            s_d3d.fence, s_d3d.fence_values[fi], s_d3d.fence_event);
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
            if (WaitForSingleObject(s_d3d.fence_event, 2000) != WAIT_TIMEOUT)
                return;
            HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
            printf("[D3D12] wait_for_gpu STUCK %ds: want %llu got %llu removed=0x%08lX\n",
                   2 * (tries + 1),
                   (unsigned long long)s_d3d.fence_values[fi],
                   (unsigned long long)s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence),
                   (long)rr);
            fflush(stdout);
            if (rr != 0 /* S_OK: device still alive, keep waiting a bit */)
                break;
        }
    }
}

/* RSX_HANG_GPU_AT_FRAME=<n> (a test): the first indexed draw of the n-th
 * batch is drawn over and over, about 4G vertices in all, so the GPU takes
 * far longer than Windows' 2 s and the driver resets it, as it does for a
 * GPU that hangs; then D3D12_DRED=1 shows what it says of a real hang (a
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
 * (ID3D12Device5::RemoveDevice) at the n-th frame, as a driver reset would
 * lose it, to see what follows: the log line, the host's dialog. Read once. */
static void lose_device_test(void)
{
    static int at = -1, frames = 0;
    if (at < 0) {
        const char* e = getenv("RSX_LOSE_DEVICE_AT_FRAME");
        at = e ? atoi(e) : 0;
    }
    if (at <= 0 || ++frames != at) return;
    ID3D12Device5* d5 = NULL;
    if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device, &IID_ID3D12Device5, (void**)&d5)) && d5) {
        fprintf(stderr, "[D3D12] RSX_LOSE_DEVICE_AT_FRAME: removing the device at frame %d%c", frames, 10);
        d5->lpVtbl->RemoveDevice(d5);
        d5->lpVtbl->Release(d5);
    }
}

static void move_to_next_frame(void)
{
    u64 current_fence = s_d3d.fence_values[s_d3d.frame_index];
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.fence, current_fence);

    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);

    lose_device_test();
    const u64 done = s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence);
    if (done == UINT64_MAX) check_device("frame fence");   /* (what a lost device's fences read) */
    if (done < s_d3d.fence_values[s_d3d.frame_index]) {
        s_d3d.fence->lpVtbl->SetEventOnCompletion(
            s_d3d.fence, s_d3d.fence_values[s_d3d.frame_index], s_d3d.fence_event);
        if (WaitForSingleObject(s_d3d.fence_event, 2000) == WAIT_TIMEOUT) {
            HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
            printf("[D3D12] FENCE STUCK 2s: want %llu got %llu removed=0x%08lX\n",
                   (unsigned long long)s_d3d.fence_values[s_d3d.frame_index],
                   (unsigned long long)s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence),
                   (long)rr);
            if (rr != S_OK) check_device("frame fence wait");
        }
    }

    s_d3d.fence_values[s_d3d.frame_index] = current_fence + 1;
}

