/* pq_mbox.h - phase 5 (2026-10-26): the persistent frame worker over a MAILBOX in shared memory.
 * Why not dspqueue: dspqueue_write answers AEE_EBADPARM on this runtime (libcdsprpc v30.0.0222) even for a
 * message-only packet - measured again today (2 040 failed writes, create/export/import all OK), exactly as the
 * house's note dspqueue-write-is-dead-on-this-runtime says. So the handoff is plain shared memory:
 *   * every rpcmem buffer of the host is mapped ONCE with fastrpc_mmap(FASTRPC_MAP_FD); mb_start gets the fds and
 *     resolves them with HAP_mmap_get; mb_stop puts them back. No FastRPC call per frame.
 *   * the mailbox (one page) holds req_seq / rsp_seq / stop and one request + one answer. The host writes the
 *     request, then req_seq; the DSP thread sees req_seq change, runs pxquake_spans exactly as the FastRPC call does,
 *     writes the answer, then rsp_seq. The host waits for rsp_seq.
 *   * caches (FASTRPC_MAP_FD = write-back on the DSP, "user maintains the caches"): the DSP invalidates the mailbox
 *     line on every look, the stream (and, for a full mirror copy, the cache) before a job, and after the job flushes
 *     AND invalidates fb, zb and the stream - no line of a buffer the CPU rewrites stays in the DSP's caches.
 *   * the wait: the thread polls with qurt_sleep. Spin (no sleep) for `spin_us` after the last job, then
 *     `poll_us` sleeps, and after 20 ms idle 1 ms sleeps (power: an idle game costs ~1000 wake-ups/s). Settings
 *     come from the host in the mailbox.
 *   * stop: the host sets mb->stop; mb_stop (FastRPC) also sets the thread's flag and JOINS it, then puts the fds.
 *     pxquake_close/release call the same stop, so the thread can never outlive the session. */
#include "qurt_memory.h"
#include "HAP_mem.h"

#define MB_MAXFD 32
#include "pq_mbox_layout.h"

static pq_mb* g_mb = NULL;
static int g_mbfd[MB_MAXFD]; static void* g_mbva[MB_MAXFD]; static int g_mbsz[MB_MAXFD]; static int g_nmb = 0;
static qurt_thread_t g_mbt; static volatile int g_mbstop = 0, g_mbrun = 0;
static volatile int g_mbdone = 0;                         /* phase 7: the thread reached its end (set before the linger) */
static pq_ctx* g_mbctx;
static inline void mb_flush(const volatile void* p, unsigned n);
/* phase 7: the idle log in the DSP's mailbox line */
static inline void mb_fill(pq_mb* mb, unsigned state, unsigned long long last_job){
    pq_ctx* c = g_mbctx;
    mb->d_state = state; mb->d_heartbeat++;
    mb->d_now_ms = (uint32_t)(HAP_perf_get_time_us() / 1000ull); mb->d_last_job_ms = (uint32_t)(last_job / 1000ull);
    mb->d_rel_total = c ? (uint32_t)c->rel_total : 0u; mb->d_rel_idle = g_rel_idle; mb->d_rel_injob = g_rel_injob;
    mb->d_vtcm_held = (c && c->ctx_id) ? 1u : 0u; mb->d_vtcm_epoch = g_vtcm_epoch; mb->d_rel_n = g_rel_n;
    for(int i = 0; i < 4; i++) mb->d_rel_t_ms[i] = g_rel_t_ms[i];
    mb->d_acq_code = c ? c->acq_code : 0;
    if(c){ mb->d_v_n = (uint32_t)c->v_n; mb->d_v_sleep = ((uint32_t)c->v_sleep_set << 8) | (uint32_t)(c->v_sleep_val & 255);
           mb->d_v_lat = (uint32_t)c->v_lat; mb->d_v_ms = (uint32_t)c->v_ms; mb->d_v_dcvs_rc = c->dcvs_rc;
           mb->d_v_lat2 = (uint32_t)c->v_lat2; mb->d_v_lat2_rc = c->v_lat2_rc; }
    mb->d_lpm_req = g_lpm_req; mb->d_v_clock = g_v_clock;
}
static inline void mb_pub(pq_mb* mb, unsigned state, unsigned long long last_job){ mb_fill(mb, state, last_job); mb_flush(&mb->rsp_seq, 128); }
static char g_mbstk[65536] __attribute__((aligned(128)));

static void* mb_va(int fd){ for(int i = 0; i < g_nmb; i++) if(g_mbfd[i] == fd) return g_mbva[i]; return NULL; }
static int mb_sz(int fd){ for(int i = 0; i < g_nmb; i++) if(g_mbfd[i] == fd) return g_mbsz[i]; return 0; }
static inline void mb_inv(const volatile void* p, unsigned n){ g_cnt_cacheops++; qurt_mem_cache_clean((qurt_addr_t)(uintptr_t)p, n, QURT_MEM_CACHE_INVALIDATE, QURT_MEM_DCACHE); }
static inline void mb_flush(const volatile void* p, unsigned n){ g_cnt_cacheops++; qurt_mem_cache_clean((qurt_addr_t)(uintptr_t)p, n, QURT_MEM_CACHE_FLUSH, QURT_MEM_DCACHE); }
static inline void mb_flinv(const volatile void* p, unsigned n){ g_cnt_cacheops++; qurt_mem_cache_clean((qurt_addr_t)(uintptr_t)p, n, QURT_MEM_CACHE_FLUSH_INVALIDATE, QURT_MEM_DCACHE); }

/* phase 9: the resource line, every 1000 jobs */
static unsigned pq_stack_hw(const void* b, unsigned size){ const unsigned char* base = (const unsigned char*)b; unsigned i = 1024; while(i < size && base[i] == 0xA5) i++; return size - i; }   /* the lowest 1 KB: QuRT data, not ours */
static void mb_res(pq_mb* mb, uint32_t jobs, unsigned maxjob_us){
    struct HAP_mem_stats ms; pq_zero(&ms, sizeof ms);
    const int rc = HAP_mem_get_stats(&ms);
    unsigned wk = 0; for(int i = 0; i < g_wn; i++){ const unsigned h = pq_stack_hw(g_wstk[i], sizeof g_wstk[i]); if(h > wk) wk = h; }
    mb->r_jobs = jobs; mb->r_heap_free_kb = (uint32_t)(ms.bytes_free >> 10); mb->r_heap_used_kb = (uint32_t)(ms.bytes_used >> 10);
    mb->r_seg_free = (uint32_t)ms.seg_free; mb->r_seg_used = (uint32_t)ms.seg_used; mb->r_mem_rc = (uint32_t)rc;
    mb->r_stack_mb = pq_stack_hw(g_mbstk, sizeof g_mbstk); mb->r_stack_wk = wk;
    mb->r_vtcm_acq = g_cnt_vtcm_acq; mb->r_votes = g_cnt_votes; mb->r_forks = g_cnt_forks; mb->r_cacheops_k = g_cnt_cacheops / 1000u;
    mb->r_dsp_ms = (uint32_t)(HAP_perf_get_time_us() / 1000ull); mb->r_maxjob_us = maxjob_us; mb->r_nmb = (uint32_t)g_nmb;
    mb->r_calls = g_mbctx ? (uint32_t)g_mbctx->calls : 0u;
    {   HAP_power_response_t g; pq_zero(&g, sizeof g); g.type = HAP_power_get_clk_Freq;   /* phase 9: the clock now */
        mb->d_clk_mhz = HAP_power_get(0, &g) == 0 ? g.clkFreqHz / 1000000u : 0xFFFFFFFFu;
        mb->d_v_clock = g_v_clock; mb_flush(&mb->rsp_seq, 128); }
    mb->r_ring_used_kb[(jobs / 1000u) & 15u] = (uint32_t)(ms.bytes_used >> 10);
    mb_flush(&mb->r_jobs, 128);
}
static void pq_mbthread(void* arg){
    (void)arg;
    pq_mb* mb = g_mb;
    uint32_t done = mb->rsp_seq;
    mb_inv(mb, 128); g_lpm_req = mb->lpm; g_lat_req = mb->lat; g_clock_req = mb->clock;   /* phase 7: -dsplpm / -dsplat; pq_vote re-votes if a job voted earlier */
    unsigned long long polls = 0, last_job = HAP_perf_get_time_us();
    uint32_t sleeps = 0, jobs = 0;
    while(!g_mbstop){
        mb_inv(mb, 128);
        if(mb->stop) break;
        const uint32_t req = mb->req_seq;
        polls++;
        if((int32_t)(req - done) <= 0 || ((mb->test_flags & 64) && jobs >= 300)){   /* TEST bit 6: stop answering after 300 jobs */
            const unsigned long long now = HAP_perf_get_time_us();
            const unsigned idle = (unsigned)(now - last_job);
            const unsigned spin = mb->spin_us, poll = mb->poll_us ? mb->poll_us : 200;
            if(idle < spin) continue;                          /* spin: lowest latency right after a job */
            if(mb->spinidle){                                  /* phase 7: -dspqspinidle, never sleep while idle */
                if((polls & 1023u) == 0) mb_pub(mb, idle > 20000 ? 3u : 1u, last_job);
                continue;
            }
            sleeps++;
            {   pq_ctx* c = g_mbctx;                          /* phase 7: a VTCM release request is honoured while idle */
                if(c && c->rel_req && idle >= 50000){        /* >= 50 ms: the host makes the next job synchronous (gap >= 40 ms) */
                    if(c->ctx_id){ pq_vtcm_release(c); g_vtcm_epoch++; g_rel_idle++; }
                    c->rel_req = 0;
                }
            }
            if(idle > 20000 || (sleeps & 15u) == 0) mb_pub(mb, idle > 20000 ? 3u : 1u, last_job);   /* the idle log, ~1 per ms */
            qurt_sleep(idle > 20000 ? 1000 : poll);
            continue;
        }
        const uint32_t seq = done + 1;
        mb_pub(mb, 2u, last_job);                          /* phase 7: in a job */
        pq_slot* sl = &mb->slot[seq % MB_SLOTS];
        mb_inv(sl, 128);                                   /* the request line */
        const int ncmd = sl->cmdsLen, ncache = sl->cacheLen, nfb = sl->fbLen, nzb = sl->zbLen, fl = sl->flags;
        unsigned char* cmds = (unsigned char*)mb_va(sl->fd_cmds); unsigned char* cache = (unsigned char*)mb_va(sl->fd_cache);
        unsigned char* fb = (unsigned char*)mb_va(sl->fd_fb); unsigned char* zb = (unsigned char*)mb_va(sl->fd_zb);
        int st[26]; for(int i = 0; i < 26; i++) st[i] = 0;
        const unsigned long long t0 = HAP_perf_get_time_us();
        if(!cmds || !cache || !fb || !zb || ncmd <= 0 || (long long)ncmd * 4 + sl->off_cmds > mb_sz(sl->fd_cmds)
           || ncache > mb_sz(sl->fd_cache) || nfb + sl->off_fb > mb_sz(sl->fd_fb) || nzb * 2 + sl->off_zb > mb_sz(sl->fd_zb)){
            st[0] = -112;                                      /* a buffer the DSP does not know, or too short */
        } else {
            cmds += sl->off_cmds; fb += sl->off_fb; zb += sl->off_zb;
            if(!(fl & 4)) mb_inv(cmds, (unsigned)ncmd * 4u);   /* bit 2: TEST without cache maintenance */
            if(fl & 1) mb_inv(cache, (unsigned)ncache);
            const unsigned long long t1 = HAP_perf_get_time_us();
            st[0] = -111;
            pxquake_spans((remote_handle64)(uintptr_t)g_mbctx, (const int*)cmds, ncmd, cache, ncache, fb, nfb, (short*)zb, nzb, st, 26);
            const unsigned long long t2 = HAP_perf_get_time_us();
            if(!(fl & 4)){ mb_flinv(fb, (unsigned)nfb); mb_flinv(zb, (unsigned)nzb * 2u); mb_inv(cmds, (unsigned)ncmd * 4u); }
            const unsigned long long t3 = HAP_perf_get_time_us();
            sl->dsp_us_inv = (int)(t1 - t0); sl->dsp_us_job = (int)(t2 - t1); sl->dsp_us_flush = (int)(t3 - t2);
        }
        for(int i = 0; i < 26; i++) sl->st[i] = st[i];
        jobs++;
        {   static unsigned maxjob = 0; const unsigned dj = (unsigned)(HAP_perf_get_time_us() - t0); if(dj > maxjob) maxjob = dj;
            if(jobs % 1000u == 0){ mb_res(mb, jobs, maxjob); maxjob = 0; } }   /* phase 9: resources every 1000 jobs */
        mb->jobs = jobs; mb->idle_sleeps = sleeps; mb->polls_lo = (uint32_t)polls; mb->polls_hi = (uint32_t)(polls >> 32);
        mb_flush(&sl->st[0], 128);                          /* the answer line */
        __asm__ __volatile__("barrier" ::: "memory");
        mb->rsp_seq = seq; done = seq;
        last_job = HAP_perf_get_time_us();
        mb_fill(mb, 1u, last_job);
        mb_flush(&mb->rsp_seq, 128);
    }
    g_mbrun = 0;
    mb_inv(mb, 128);
    {   pq_ctx* c = g_mbctx; if(c && c->ctx_id){ pq_vtcm_release(c); g_vtcm_epoch++; } }   /* phase 7: the VTCM back first */
    if(mb->test_flags & 16) pq_spin_us(5000000);         /* TEST: the ack 5 s late (spinning) */
    mb_fill(mb, 4u, last_job); mb->d_stop_ack = 0x53544F50u; mb_flush(&mb->rsp_seq, 128);   /* "STOP" acked in shared memory */
    if(!(mb->test_flags & 2)) while(!g_mbstop){}         /* the RPC stop joins this thread - spinning, never sleeping (bit 1 TEST: leave at once) */
    if(mb->test_flags & 8) pq_spin_us(1500000);          /* TEST: a stop that takes 1.5 s (spinning) */
    g_mbdone = 1;                                        /* phase 7: the stopper may join now */
    if(!(mb->test_flags & 2)) pq_spin_us(2000);          /* linger (spinning): the stopper's qurt_thread_join must be in place, else it returns
                                                            QURT_ENOTHREAD and the exit code is lost */
    g_join_testflags = mb->test_flags;                   /* TEST: bit 2 reaches the HVX workers (joined after this thread) */
    if(mb->test_flags & 1) return;                       /* known-sick: no exit code */
    qurt_thread_exit(0x4A4F);                            /* "JO": the witness of a clean end */
}

static int g_mb_join_rc = -100, g_mb_exit = 0;          /* -100 = no thread was running */
static int g_mb_timeouts = 0;                             /* phase 7: stops that hit the bound */
static int pq_mbstop_internal(void){
    int js = 0x7FFFFFFF;
    if(g_mbt){
        g_mbstop = 1;
        const unsigned long long t0 = HAP_perf_get_time_us();
        while(!g_mbdone && HAP_perf_get_time_us() - t0 < 1000000ull) pq_spin_us(50);   /* bounded: 1 s, spinning */
        if(!g_mbdone){                                   /* still running: no join, its mappings stay (it may use them) */
            g_mb_join_rc = -101; g_mb_exit = 0; g_mb_timeouts++;
            return js;
        }
        g_mb_join_rc = qurt_thread_join((unsigned)g_mbt, &js); g_mb_exit = js; g_mbt = 0;
    }
    else { g_mb_join_rc = -100; g_mb_exit = 0; }
    for(int i = 0; i < g_nmb; i++) HAP_mmap_put(g_mbfd[i]);
    g_nmb = 0; g_mb = NULL; g_mbrun = 0;
    return js;
}

int pxquake_mb_start(remote_handle64 h, const int* fds, int fdsLen, const int* sizes, int sizesLen, int* status, int statusLen){
    pq_ctx* c = pq_get(h);
    if(!c || statusLen < 4 || fdsLen < 1 || fdsLen > MB_MAXFD || sizesLen != fdsLen) return AEE_EBADPARM;
    if(g_mbt){ status[0] = -1; return AEE_SUCCESS; }
    g_nmb = 0;
    for(int i = 0; i < fdsLen; i++){
        void* va = NULL; uint64 pa = 0;
        int r = HAP_mmap_get(fds[i], &va, &pa);
        if(r || !va){ status[0] = -2; status[1] = i; status[2] = r; pq_mbstop_internal(); return AEE_SUCCESS; }
        g_mbfd[g_nmb] = fds[i]; g_mbva[g_nmb] = va; g_mbsz[g_nmb] = sizes[i]; g_nmb++;
    }
    g_mb = (pq_mb*)g_mbva[0];                                 /* fds[0] = the mailbox */
    mb_inv(g_mb, sizeof(pq_mb));
    g_mbctx = c; g_mbstop = 0; g_mbrun = 1; g_mbdone = 0;
    g_mb->alive = 1; g_mb->d_stop_ack = 0; g_mb->d_state = 1; mb_flush(&g_mb->rsp_seq, 128);
    const int prio = qurt_thread_get_priority(qurt_thread_get_id());
    qurt_thread_attr_t at; qurt_thread_attr_init(&at);
    memset(g_mbstk, 0xA5, sizeof g_mbstk);               /* phase 9: high-water mark */
    qurt_thread_attr_set_stack_addr(&at, g_mbstk); qurt_thread_attr_set_stack_size(&at, sizeof g_mbstk);
    qurt_thread_attr_set_priority(&at, (unsigned short)(prio > 1 ? prio : 100));
    if(qurt_thread_create(&g_mbt, &at, pq_mbthread, NULL) != 0){ g_mbt = 0; status[0] = -3; pq_mbstop_internal(); return AEE_SUCCESS; }
    status[0] = 0x73; status[1] = g_nmb; status[2] = prio; status[3] = (int)(uintptr_t)g_mb;
    return AEE_SUCCESS;
}
int pxquake_mb_stop(remote_handle64 h, int* status, int statusLen){
    unsigned jobs = 0, sl = 0, recyc = 0; unsigned long long polls = 0;
    if(g_mb){ mb_inv(g_mb, sizeof(pq_mb)); recyc = g_mb->recycle; jobs = g_mb->jobs; sl = g_mb->idle_sleeps; polls = ((unsigned long long)g_mb->polls_hi << 32) | g_mb->polls_lo; }
    pq_mbstop_internal();
    if((recyc & 2u) && g_mb_join_rc == 0) pq_pool_down();   /* phase 9 experiment B: new HVX workers with the next job */
    if(statusLen > 15){ status[12] = (int)g_cnt_vtcm_acq; status[13] = g_pool_joined; status[14] = g_pool_clean; status[15] = g_pool_bad; }
    if(statusLen > 0) status[0] = 0x74;
    if(statusLen > 1) status[1] = g_mb_join_rc;           /* QURT_EOK (0) = joined; -100 = no thread was running; -101 = the 1 s bound expired */
    if(statusLen > 5) status[5] = g_mb_exit;              /* 0x4A4F = the thread ended through qurt_thread_exit */
    if(statusLen > 2) status[2] = (int)jobs;
    if(statusLen > 3) status[3] = (int)sl;
    if(statusLen > 4) status[4] = (int)polls;
    {   pq_ctx* c = g_mbctx ? g_mbctx : pq_get(h);         /* phase 7: the applied vote */
        if(c && statusLen > 11){ status[6] = c->v_n; status[7] = (c->v_sleep_set << 8) | (c->v_sleep_val & 255); status[8] = c->dcvs_rc;
                                 status[9] = (int)g_lpm_req; status[10] = c->v_lat2; status[11] = c->v_lat2_rc; } }
    return AEE_SUCCESS;
}
