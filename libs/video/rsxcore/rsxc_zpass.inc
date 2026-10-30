/* -------------------------------------------------------------------------
 * Occlusion queries (NV4097 ZPASS pixel counts)
 *
 * A title counts the samples an object would cover -- the sun's probe quad,
 * say -- between CLEAR_REPORT_VALUE and GET_REPORT, with
 * SET_ZPASS_PIXEL_COUNT_ENABLE on, and reads the count back with
 * cellGcmGetReport. Answering "visible" every time drew Demon's Souls's sun
 * flare through walls. Each draw made while counting is on gets a D3D12
 * occlusion query; once the GPU has finished the frame the counts are summed
 * between the clear and the report, in stream order, and stored in the report.
 * The answer lands a frame later than on hardware, which a flare fading in
 * and out does not notice.
 *
 * Events are recorded with the record index they precede (draw_count at the
 * time), and draws are matched by cb_slot, their record index, so the debug
 * passes that reorder records do not move a query. RSX_ZPASS=0 goes back to
 * reporting "visible" (read at init).
 * -----------------------------------------------------------------------*/
extern void (*g_rsx_report_hook)(u32 method, u32 data);
#define ZQ_MAX     256    /* counted draws per batch */
#define ZQ_EV_MAX  256    /* report events per batch */
enum { ZQ_OFF = 0, ZQ_ON = 1, ZQ_CLEAR = 2, ZQ_REPORT = 3 };
static ID3D12QueryHeap* s_zq_heap;
static ID3D12Resource*  s_zq_rb;
static int  s_zq_on;                       /* counting enabled, as of the stream */
static int  s_zq_on_start;                 /* ... when the current batch began */
static struct { u32 at, kind, idx; } s_zq_ev[ZQ_EV_MAX];
static int  s_zq_nev;
static u32  s_zq_rec[ZQ_MAX];              /* cb_slot of each query issued */
static float s_zq_area[ZQ_MAX];            /* its target's host/guest pixel ratio (RSX_SCALE) */
static int  s_zq_n;                        /* queries issued this batch */
static u64  s_zq_acc;                      /* samples since the last clear */

static void zq_hook(u32 method, u32 data)
{
    u32 kind, idx = 0;
    if (method == 0x17CC) { s_zq_on = data != 0; kind = s_zq_on ? ZQ_ON : ZQ_OFF; }
    else if (method == 0x17C8) { if (data != 1) return; kind = ZQ_CLEAR; }
    else { if ((data >> 24) != 1) return; kind = ZQ_REPORT; idx = (data & 0xFFFFFFu) / 16u; }
    if (s_zq_nev < ZQ_EV_MAX) {
        s_zq_ev[s_zq_nev].at = s_d3d.draw_count;
        s_zq_ev[s_zq_nev].kind = kind;
        s_zq_ev[s_zq_nev].idx = idx;
        s_zq_nev++;
    }
}

/* Was counting on when record `slot` was made? */
static int zq_counted(u32 slot)
{
    int on = s_zq_on_start;
    for (int i = 0; i < s_zq_nev && s_zq_ev[i].at <= slot; i++)
        if (s_zq_ev[i].kind <= ZQ_ON) on = (int)s_zq_ev[i].kind;
    return on;
}

static int zq_ready(void)
{
    if (s_zq_heap) return 1;
    D3D12_QUERY_HEAP_DESC qd = {0};
    qd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    qd.Count = ZQ_MAX;
    if (FAILED(s_d3d.device->lpVtbl->CreateQueryHeap(s_d3d.device, &qd,
            &IID_ID3D12QueryHeap, (void**)&s_zq_heap))) { s_zq_heap = NULL; return 0; }
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = ZQ_MAX * sizeof(u64);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(d3d_create_committed(s_d3d.device, &hp,
            D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_zq_rb))) {
        s_zq_heap->lpVtbl->Release(s_zq_heap); s_zq_heap = NULL; s_zq_rb = NULL; return 0;
    }
    return 1;
}

/* After the batch's GPU work is complete: sum the counts in stream order and
 * store each report. ZPASS_LOG=<n> prints the first n stored values. */
static void zq_finish(void)
{
    if (!s_zq_nev && !s_zq_n) { s_zq_on_start = s_zq_on; return; }
    u64 cnt[ZQ_MAX];
    memset(cnt, 0, sizeof cnt);
    if (s_zq_n) {
        void* mp = NULL;
        D3D12_RANGE rr = {0, (SIZE_T)s_zq_n * sizeof(u64)};
        if (SUCCEEDED(s_zq_rb->lpVtbl->Map(s_zq_rb, 0, &rr, &mp)) && mp) {
            memcpy(cnt, mp, (size_t)s_zq_n * sizeof(u64));
            D3D12_RANGE wr = {0, 0};
            s_zq_rb->lpVtbl->Unmap(s_zq_rb, 0, &wr);
        }
        /* A scaled target covers more samples; report what the guest's size would,
         * rounded up so a sliver that showed is never reported as hidden. */
        for (int i = 0; i < s_zq_n; i++)
            if (s_zq_area[i] > 1.0f && cnt[i]) cnt[i] = (u64)ceil((double)cnt[i] / s_zq_area[i]);
    }
    extern void cellGcm_report_store(u32 index, u32 value);
    static int logn = -1, logged = 0;
    PS3_KNOB(logn, ps3_env_int("ZPASS_LOG", 0));
    int qi = 0;
    for (int e = 0; e < s_zq_nev; e++) {
        for (; qi < s_zq_n && s_zq_rec[qi] < s_zq_ev[e].at; qi++) s_zq_acc += cnt[qi];
        if (s_zq_ev[e].kind == ZQ_CLEAR) s_zq_acc = 0;
        else if (s_zq_ev[e].kind == ZQ_REPORT) {
            const u32 v = s_zq_acc > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)s_zq_acc;
            cellGcm_report_store(s_zq_ev[e].idx, v);
            if (logged < logn) { logged++;
                fprintf(stderr, "[ZPASS] report %u = %u (%d queries this batch)%c",
                        s_zq_ev[e].idx, v, s_zq_n, 10); }
        }
    }
    for (; qi < s_zq_n; qi++) s_zq_acc += cnt[qi];
    s_zq_nev = 0;
    s_zq_n = 0;
    s_zq_on_start = s_zq_on;
}

