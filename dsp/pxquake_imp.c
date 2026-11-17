/* pxquake_imp.c - DSP side of PulseX Quake phase 2 (2026-10-26): Quake 1 span drawing on the Hexagon cDSP.
 *
 * Its own small FastRPC module (libpxquake_skel.so), built by build_skel.sh, signed by sign_pxquake.ps1.
 * Host side: PulseX/bridge/pxquake/pc_dsp.c (the writer of the command stream).
 *
 * BIT-EXACTNESS DESIGN. id's D_DrawSpans8 does a float division (z = 65536/zi) per 8-pixel segment, with float
 * products and clamps; D_DrawZSpans does double products. All of that floating point stays on the ARM CPU, in the
 * same C expressions as id's code compiled with the same flags (-ffp-contract=off). The CPU writes, per segment, the
 * integer start s, t and integer steps sstep, tstep; per span the integer izi and izistep. This file does ONLY the
 * integer stepping, the texel fetches and the z writes - integer arithmetic has one answer on every machine, so the
 * DSP's float rounding and FMA contraction never enter.
 *
 * COMMAND STREAM (int32 words, cmds[]):
 *   [1, cache_off, cachewidth]                   SURF: texel base = cache + cache_off (byte offset of the surface
 *                                                  block's data inside the surface cache), row width in texels
 *   [2, fb_off, z_off, count, izi, izistep,      SPAN: dest = fb + fb_off, z dest = zb + z_off (shorts),
 *    then ceil(count/8) x (s, t, sstep, tstep)]   count pixels; segment k has 8 pixels except the last (count - 8k)
 *   [0]                                          END
 * Per pixel (id's inner loop):  *pdest++ = pbase[(s >> 16) + (t >> 16) * cachewidth]; s += sstep; t += tstep;
 * Z (id's D_DrawZSpans, including its 2-pixel packing, which differs from per-pixel for negative izi):
 *   if (address & 2) { one short }  pairs packed as (izi>>16) | (izi' & 0xFFFF0000)  odd tail one short.
 * Signed overflow in "izi += izistep" is done in unsigned arithmetic (= what the ARM code does in practice).
 *
 * GUARDS: every span's destination range and every texel index are checked against the buffer lengths; a violation
 * is skipped and COUNTED (status[5], status[8]) - never a silent read outside the mapped memory.
 */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "HAP_perf.h"
#include "HAP_power.h"
#include "HAP_mem.h"	/* phase 9: HAP_mem_get_stats */
#include "HAP_compute_res.h"
#include "AEEStdErr.h"
#include "pxquake.h"
#include "pq_kern.h"
#include <hexagon_protos.h>
#include "qurt_thread.h"
#include "qurt_signal.h"

#define PQ_MAGIC 0x5051554Bu   /* "PQUK" */
typedef struct { unsigned magic; int voted; int calls; int dcvs_rc;
                 unsigned ctx_id; compute_res_attr_t attr; unsigned char* vtcm; unsigned vtcm_bytes; volatile int rel_req; int acq_code; int hvx_rc; int mirror_ok;
                 volatile int rel_total;
                 int v_n, v_sleep_set, v_sleep_val, v_lat, v_ms, v_lat2, v_lat2_rc; unsigned v_lpm_req, v_lat_req, v_clk_req; } pq_ctx;   /* phase 7: the applied vote */   /* phase 7: VTCM release requests from other clients, all session */
/* VTCM for the staged surface blocks (op 4). Cooperative preemption as living/dsp: the callback only sets a flag;
 * the next call gives the VTCM back and draws from the shared cache. */
static volatile unsigned g_rel_n = 0, g_rel_t_ms[4], g_rel_injob = 0, g_rel_idle = 0, g_vtcm_epoch = 0;   /* phase 7: the idle log */
static volatile unsigned g_cnt_vtcm_acq = 0, g_cnt_votes = 0, g_cnt_forks = 0, g_cnt_cacheops = 0;   /* phase 9: what could grow */
static int pq_release_cb(unsigned int context, void* state){
    pq_ctx* c = (pq_ctx*)state;
    if(!c || c->ctx_id != context) return -1;
    c->rel_req++; c->rel_total++;
    g_rel_t_ms[g_rel_n & 3u] = (unsigned)(HAP_perf_get_time_us() / 1000ull); g_rel_n++;
    return 0;
}
static void pq_vtcm_release(pq_ctx* c){
    if(c->ctx_id) HAP_compute_res_release(c->ctx_id);
    c->ctx_id = 0; c->vtcm = 0; c->vtcm_bytes = 0; c->mirror_ok = 0;
}
static int pq_vtcm_ensure(pq_ctx* c){
    if(c->rel_req){ pq_vtcm_release(c); c->rel_req = 0; g_rel_injob++; g_vtcm_epoch++; return -3; }
    if(c->ctx_id) return 0;
    static const unsigned trys[4] = { 8u<<20, 6u<<20, 4u<<20, 2u<<20 };
    for(int i = 0; i < 4 && !c->ctx_id; i++){
        HAP_compute_res_attr_init(&c->attr);
        HAP_compute_res_attr_set_vtcm_param(&c->attr, trys[i], 1);
        HAP_compute_res_attr_set_release_callback(&c->attr, pq_release_cb, (void*)c);
        c->ctx_id = HAP_compute_res_acquire(&c->attr, 1000000); g_cnt_vtcm_acq++;
        if(c->ctx_id){ c->vtcm_bytes = trys[i]; c->acq_code = (int)(trys[i] >> 20); }
    }
    if(!c->ctx_id){ c->acq_code = -1; return -1; }
    c->vtcm = (unsigned char*)HAP_compute_res_attr_get_vtcm_ptr(&c->attr);
    if(!c->vtcm){ HAP_compute_res_release(c->ctx_id); c->ctx_id = 0; c->vtcm_bytes = 0; c->acq_code = -2; return -2; }
    return 0;
}

static pq_ctx* pq_get(remote_handle64 h){ pq_ctx* c = (pq_ctx*)(uintptr_t)h; return (c && c->magic == PQ_MAGIC) ? c : 0; }
static void pq_zero(void* p, unsigned n){ unsigned char* b = (unsigned char*)p; while(n--) *b++ = 0; }

static volatile unsigned g_lpm_req = 0;            /* phase 7: -dsplpm (the mailbox thread sets it before its first job) */
static volatile unsigned g_lat_req = 0;            /* phase 7: -dsplat, lever 2 */
static volatile unsigned g_clock_req = 0;          /* phase 9: -dspclock, 0 turbo 1 nom 2 dcvs */
static volatile unsigned g_v_clock = 0;            /* phase 9: what pq_vote applied (see pq_mb.d_v_clock) */
static int g_lat_ctx;                              /* lever 2's own HAP power context */
static void pq_vote(pq_ctx* c){
    const unsigned want = g_lpm_req, want_lat = g_lat_req, want_clk = g_clock_req > 2u ? 0u : g_clock_req;
    if(c->voted && c->v_lpm_req == want && c->v_lat_req == want_lat && c->v_clk_req == want_clk) return;   /* phase 7: re-vote when the request changed */
    /* the house clock vote (living/dsp, pulsehmx): DCVS off, core and bus at TURBO, 100 us latency */
    HAP_power_request_t r; pq_zero(&r, sizeof r); r.type = HAP_power_set_DCVS_v3;
    r.dcvs_v3.set_dcvs_enable = 1; r.dcvs_v3.dcvs_enable = 0; r.dcvs_v3.dcvs_option = HAP_DCVS_V2_PERFORMANCE_MODE;
    r.dcvs_v3.set_latency = 1; r.dcvs_v3.latency = 100;
    r.dcvs_v3.set_core_params = 1; r.dcvs_v3.core_params.target_corner = HAP_DCVS_VCORNER_TURBO;
    r.dcvs_v3.core_params.min_corner = HAP_DCVS_VCORNER_TURBO; r.dcvs_v3.core_params.max_corner = HAP_DCVS_VCORNER_MAX;
    r.dcvs_v3.set_bus_params = 1; r.dcvs_v3.bus_params.target_corner = HAP_DCVS_VCORNER_TURBO;
    r.dcvs_v3.bus_params.min_corner = HAP_DCVS_VCORNER_TURBO; r.dcvs_v3.bus_params.max_corner = HAP_DCVS_VCORNER_MAX;
    if(want_clk == 1u){                                /* phase 9 experiment A: NOM, still fixed (DCVS off) */
        r.dcvs_v3.core_params.target_corner = HAP_DCVS_VCORNER_NOM; r.dcvs_v3.core_params.min_corner = HAP_DCVS_VCORNER_NOM;
        r.dcvs_v3.bus_params.target_corner = HAP_DCVS_VCORNER_NOM; r.dcvs_v3.bus_params.min_corner = HAP_DCVS_VCORNER_NOM;
    } else if(want_clk == 2u){                         /* phase 9 experiment A: DCVS on, performance mode, SVS..TURBO */
        r.dcvs_v3.dcvs_enable = 1;
        r.dcvs_v3.core_params.target_corner = HAP_DCVS_VCORNER_TURBO; r.dcvs_v3.core_params.min_corner = HAP_DCVS_VCORNER_SVS;
        r.dcvs_v3.core_params.max_corner = HAP_DCVS_VCORNER_TURBO;
        r.dcvs_v3.bus_params.target_corner = HAP_DCVS_VCORNER_TURBO; r.dcvs_v3.bus_params.min_corner = HAP_DCVS_VCORNER_SVS;
        r.dcvs_v3.bus_params.max_corner = HAP_DCVS_VCORNER_TURBO;
    }
    if(want || c->v_sleep_set){ r.dcvs_v3.set_sleep_disable = 1; r.dcvs_v3.sleep_disable = (unsigned char)want; }   /* phase 7: -dsplpm (0 undoes it) */
    c->dcvs_rc = HAP_power_set((void*)c, &r); g_cnt_votes++;
    c->v_sleep_set = (want || c->v_sleep_set) ? 1 : 0; c->v_sleep_val = (int)want; c->v_lat = 100;
    if(want_lat || c->v_lat2){                         /* lever 2: an independent wakeup-latency vote (mips_bw) */
        HAP_power_request_t m; pq_zero(&m, sizeof m); m.type = HAP_power_set_DCVS_v3;   /* the mips_bw form: 0x8000040E */
        m.dcvs_v3.set_latency = 1; m.dcvs_v3.latency = want_lat ? want_lat : 65535;
        c->v_lat2_rc = HAP_power_set((void*)&g_lat_ctx, &m); c->v_lat2 = (int)want_lat;
    }
    {   HAP_power_response_t g; pq_zero(&g, sizeof g); g.type = HAP_power_get_dcvsEnabled;   /* phase 9: the witness */
        const int grc = HAP_power_get((void*)c, &g);
        g_v_clock = want_clk | (grc == 0 && g.dcvsEnabled ? 0x100u : 0u) | (grc != 0 ? 0x8000u : 0u); }
    c->v_clk_req = want_clk;
    c->v_lpm_req = want; c->v_lat_req = want_lat; c->v_n++; c->v_ms = (int)(HAP_perf_get_time_us() / 1000ull);
    if(c->voted) return;                               /* a re-vote: HVX is on already */
#ifndef PQ_NO_HVX_VOTE
    { HAP_power_request_t q; pq_zero(&q, sizeof q); q.type = HAP_power_set_HVX; q.hvx.power_up = 1; c->hvx_rc = HAP_power_set((void*)c, &q); }
#endif
    c->voted = 1;
}
static int pq_unvote(pq_ctx* c){
    if(!c->voted) return 0;
    if(c->v_lat2){ HAP_power_request_t m; pq_zero(&m, sizeof m); m.type = HAP_power_set_DCVS_v3;   /* phase 7: lever 2 off */
                   m.dcvs_v3.set_latency = 1; m.dcvs_v3.latency = 65535; HAP_power_set((void*)&g_lat_ctx, &m); c->v_lat2 = 0; }
    HAP_power_request_t r; pq_zero(&r, sizeof r); r.type = HAP_power_set_DCVS_v3;
    r.dcvs_v3.set_dcvs_enable = 1; r.dcvs_v3.dcvs_enable = 1; r.dcvs_v3.dcvs_option = HAP_DCVS_V2_POWER_SAVER_MODE;
    r.dcvs_v3.set_latency = 1; r.dcvs_v3.latency = 65535;
    r.dcvs_v3.set_core_params = 1; r.dcvs_v3.set_bus_params = 1; r.dcvs_v3.set_dcvs_v3_params = 1; r.dcvs_v3.set_sleep_disable = 1;
    { HAP_power_request_t q; pq_zero(&q, sizeof q); q.type = HAP_power_set_HVX; q.hvx.power_up = 0; HAP_power_set((void*)c, &q); }
    int rc = HAP_power_set((void*)c, &r);
    c->voted = 0;
    return rc;
}

static void pq_pool_down(void);
/* phase 7: a timed wait that never sleeps - a DSP whose threads all wait on timers stopped running (measured) */
static void pq_spin_us(unsigned us){ const unsigned long long t0 = HAP_perf_get_time_us(); while(HAP_perf_get_time_us() - t0 < us){} }
static volatile unsigned g_join_testflags = 0;     /* TEST: copied from the mailbox by its thread at its end; bit 2 = workers skip exit */
static int g_pool_joined = 0, g_pool_clean = 0, g_pool_bad = 0, g_pool_last_jr = 0, g_pool_last_js = 0;   /* the HVX workers' join witness */
static int g_pool_timeouts = 0;                    /* phase 7: workers that did not reach their end within the bound */
static int pq_qstop_internal(void);
static int pq_mbstop_internal(void);
int pxquake_open(const char* uri, remote_handle64* h){
    (void)uri;
    pq_ctx* c = (pq_ctx*)calloc(1, sizeof(pq_ctx));
    if(!c) return AEE_ENOMEMORY;
    c->magic = PQ_MAGIC;
    *h = (remote_handle64)(uintptr_t)c;
    return AEE_SUCCESS;
}
int pxquake_close(remote_handle64 h){
    pq_ctx* c = pq_get(h);
    pq_qstop_internal();                   /* phase 5: the queue thread never outlives the session */
    pq_mbstop_internal();
    pq_pool_down();
    if(c){ pq_vtcm_release(c); pq_unvote(c); c->magic = 0; free(c); }
    return AEE_SUCCESS;
}
int pxquake_release(remote_handle64 h, int* status, int statusLen){
    pq_ctx* c = pq_get(h);
    if(!c) return AEE_EBADPARM;
    pq_qstop_internal();
    pq_mbstop_internal();
    pq_pool_down();
    pq_vtcm_release(c);
    int rc = pq_unvote(c);
    if(statusLen > 0) status[0] = 364;
    if(statusLen > 1) status[1] = rc;
    if(statusLen > 2) status[2] = c->calls;
    if(statusLen > 3) status[3] = g_pool_joined;         /* HVX workers joined in this session */
    if(statusLen > 4) status[4] = g_pool_clean;          /* ... with QURT_EOK and exit code 0x574B */
    if(statusLen > 5) status[5] = g_pool_bad;
    if(statusLen > 6) status[6] = g_pool_last_jr;
    if(statusLen > 7) status[7] = g_pool_last_js;
    if(statusLen > 8) status[8] = g_pool_timeouts;       /* phase 7: workers past the bound */
    if(statusLen > 9) status[9] = c->rel_total;          /* phase 7: VTCM release requests seen */
    return AEE_SUCCESS;
}
int pxquake_nop(remote_handle64 h){
    pq_ctx* c = pq_get(h);
    if(!c) return AEE_EBADPARM;
    pq_vote(c);
    c->calls++;
    return AEE_SUCCESS;
}
int pxquake_touch(remote_handle64 h, const unsigned char* buf, int bufLen, int* status, int statusLen){
    pq_ctx* c = pq_get(h);
    if(!c || statusLen < 4) return AEE_EBADPARM;
    pq_vote(c);
    c->calls++;
    status[0] = 0x51; status[1] = bufLen > 0 ? buf[0] : -1; status[2] = bufLen > 0 ? buf[bufLen - 1] : -1; status[3] = bufLen;
    /* memory probe (buf[0] == 0x77, statusLen >= 10): is the shared buffer slow to touch from the DSP?
     * [4] us byte-sum over the shared buffer, [5] us memcpy shared -> DSP heap, [6] us byte-sum over the heap copy,
     * [7] us strided (64 B) byte reads shared, [8] the sums agree (1/0), [9] us memcpy heap -> shared */
    if(bufLen > 0 && buf[0] == 0x77 && statusLen >= 10){
        unsigned char* loc = (unsigned char*)malloc((unsigned)bufLen);
        if(loc){
            unsigned long long a = HAP_perf_get_time_us(); unsigned s1 = 0, s2 = 0, s3 = 0;
            for(int i = 0; i < bufLen; i++) s1 += buf[i];
            unsigned long long b = HAP_perf_get_time_us();
            memcpy(loc, buf, (unsigned)bufLen);
            unsigned long long d = HAP_perf_get_time_us();
            for(int i = 0; i < bufLen; i++) s2 += loc[i];
            unsigned long long e = HAP_perf_get_time_us();
            for(int i = 0; i < bufLen; i += 64) s3 += buf[(i * 7919) % bufLen];
            unsigned long long f = HAP_perf_get_time_us();
            unsigned long long g = HAP_perf_get_time_us();
            status[4] = (int)(b - a); status[5] = (int)(d - b); status[6] = (int)(e - d); status[7] = (int)(f - e) + (int)(s3 & 0); status[8] = s1 == s2; status[9] = (int)(g - f);
            free(loc);
        }
    }
    return AEE_SUCCESS;
}

/* ---------------------------------------------------------------- multithreaded path (op 3 with nthreads >= 1)
 * Pass 1 (this thread) walks the stream once and indexes every span with its surface. Pass 2 hands span j to
 * thread j mod T. Spans of one frame never overlap on screen (Quake's edge sort), so the threads write disjoint
 * pixels; the per-pixel arithmetic is the serial path's, so the bytes are the same.
 * Pool: the house pattern (living/dsp): static stacks, priority 100, signals go/done; torn down by release/close. */
#define PQ_MAXT 6
typedef struct { qurt_thread_t tid; qurt_signal_t go, done; volatile int quit, idx; volatile int ended; } pq_worker;
static pq_worker g_w[PQ_MAXT - 1];
static char g_wstk[PQ_MAXT - 1][16384] __attribute__((aligned(128)));
static int g_wn = 0;
typedef struct {
    const int* cmds; const int* ix; int nix; int T;
    const unsigned char* cache; int cacheLen; const unsigned char* vtcm; unsigned char* fb; int fbLen; short* zb; int zbLen;
    int npix[PQ_MAXT], oob[PQ_MAXT], oobd[PQ_MAXT]; int diag; int texpf;
    int phase; int recpf; const int* bnd; int mirror; int inl; int ndirty; const int* dirty; const int* stab; const int* sidx; int nsurf; unsigned char* vt;
    int hvx; unsigned char* gs; const int* cmds_end; int thvx[PQ_MAXT], tfb[PQ_MAXT], zhvx[PQ_MAXT], zsc[PQ_MAXT];
    const int* tul; int ntu; int tab_off; int tb[PQ_MAXT + 1];          /* phase 6: the water spans, thread k does [tb[k], tb[k+1]) */
    unsigned char* tv; int tuh[PQ_MAXT], tus[PQ_MAXT];                 /* VTCM water areas (0 = scalar); spans by HVX / scalar */
    int tus_us[PQ_MAXT];                                               /* us of each thread's water part */
} pq_job;
static pq_job* volatile g_job = 0;
static void pq_band(pq_job* J, int k);
static void pq_turb_span(const int* cmds, int tab_off, int pos, unsigned char* fb, int fbLen, short* zb, int zbLen, int* npix, int* bad);
static void pq_turb_band3(pq_job* J, int k, int* p, int* b, int* h, int* sc);
#define PQ_TV 5632                                         /* per thread: (unused 512) + texture 4096 + pad 128 + 4 gather targets + 3 vlut tables */
static void pq_worker_main(void* arg){
    pq_worker* w = (pq_worker*)arg;
    for(;;){
        qurt_signal_wait(&w->go, 1u, QURT_SIGNAL_ATTR_WAIT_ANY);
        qurt_signal_clear(&w->go, 1u);
        if(w->quit) break;
        pq_band(g_job, w->idx);
        qurt_signal_set(&w->done, 1u);
    }
    w->ended = 1;                                       /* phase 7: the stopper may join now */
    pq_spin_us(2000);                                   /* linger (spinning): the join must be in place (else QURT_ENOTHREAD, no exit code) */
    if(g_join_testflags & 4) return;                    /* known-sick: no exit code */
    qurt_thread_exit(0x574B);                           /* "WK": the witness of a clean end */
}
static void pq_pool_down(void){
    for(int i = 0; i < g_wn; i++){
        g_w[i].quit = 1; qurt_signal_set(&g_w[i].go, 1u);   /* one at a time: released, then joined */
        {   const unsigned long long t0 = HAP_perf_get_time_us();      /* phase 7: bounded, 0.5 s */
            while(!g_w[i].ended && HAP_perf_get_time_us() - t0 < 500000ull) pq_spin_us(50);
            if(!g_w[i].ended){ g_pool_joined++; g_pool_bad++; g_pool_timeouts++; g_pool_last_jr = -101; g_pool_last_js = 0; continue; }   /* no join, signals kept */
        }
        int js = 0x7FFFFFFF; const int jr = qurt_thread_join((unsigned)g_w[i].tid, &js);
        g_pool_joined++;
        if(jr == 0 /* QURT_EOK */ && js == 0x574B) g_pool_clean++; else { g_pool_bad++; g_pool_last_jr = jr; g_pool_last_js = js; }
        qurt_signal_destroy(&g_w[i].go); qurt_signal_destroy(&g_w[i].done);
    }
    g_wn = 0;
}
static int pq_pool_up(int n){
    if(n > PQ_MAXT - 1) n = PQ_MAXT - 1;
    if(g_wn >= n) return g_wn;
    for(int i = g_wn; i < n; i++){
        g_w[i].quit = 0; g_w[i].idx = i + 1; g_w[i].ended = 0;
        qurt_signal_init(&g_w[i].go); qurt_signal_init(&g_w[i].done);
        qurt_thread_attr_t at; qurt_thread_attr_init(&at);
        memset(g_wstk[i], 0xA5, sizeof g_wstk[i]);       /* phase 9: high-water mark */
        qurt_thread_attr_set_stack_addr(&at, g_wstk[i]); qurt_thread_attr_set_stack_size(&at, sizeof g_wstk[i]);
        qurt_thread_attr_set_priority(&at, 100);
        if(qurt_thread_create(&g_w[i].tid, &at, pq_worker_main, &g_w[i]) != 0){ qurt_signal_destroy(&g_w[i].go); qurt_signal_destroy(&g_w[i].done); break; }
        g_wn = i + 1;
    }
    return g_wn;
}
static inline void pq_texpf(const pq_job* J, int j){
    const int* sp = J->cmds + J->ix[5 * j];
    const int base_off = J->ix[5 * j + 1], cw = J->ix[5 * j + 2], count = sp[3];
    if(count <= 0 || cw <= 0) return;
    const int nseg = (count + 7) >> 3; const int* a = sp + 6; const int* b = sp + 6 + 4 * (nseg - 1);
    const int nl = count - 8 * (nseg - 1);
    const long long se = (long long)b[0] + (long long)(nl - 1) * b[2], te = (long long)b[1] + (long long)(nl - 1) * b[3];
    long long s0 = a[0] < se ? a[0] : se, s1 = a[0] < se ? se : a[0], t0 = a[1] < te ? a[1] : te, t1 = a[1] < te ? te : a[1];
    s0 >>= 16; s1 >>= 16; t0 >>= 16; t1 >>= 16;
    if(s0 < 0 || t0 < 0) return;
    long long w = s1 - s0 + 1, hgt = t1 - t0 + 1;
    if(w > 255) w = 255; if(hgt > 255) hgt = 255; if(cw > 65535) return;
    const long long off = (long long)base_off + t0 * cw + s0;
    if(off < 0 || off >= J->cacheLen) return;
    Q6_l2fetch_AR((void*)(J->cache + off), ((unsigned)cw << 16) | ((unsigned)w << 8) | (unsigned)hgt);
}
typedef long pq_uvec1 __attribute__((__vector_size__(128))) __attribute__((aligned(1)));
static void pq_stage(pq_job* J, int k){
    if(J->mirror){
        if(J->ndirty < 0){                                  /* everything: chunk k of T */
            const int per = ((J->cacheLen + J->T - 1) / J->T + 127) & ~127; const int a = per * k;
            int n = J->cacheLen - a; if(n > per) n = per;
            if(n > 0) memcpy(J->vt + a, J->cache + a, (unsigned)n);
            return;
        }
        if(J->inl){ for(int i = k; i < J->ndirty; i += J->T){ const int* d = J->dirty + 3 * i; memcpy(J->vt + d[0], (const unsigned char*)(J->cmds + d[2]), (unsigned)d[1]); } }
        else for(int i = k; i < J->ndirty; i += J->T){ const int* d = J->dirty + 2 * i; memcpy(J->vt + d[0], J->cache + d[0], (unsigned)d[1]); }
        return;
    }
    for(int i = k; i < J->nsurf; i += J->T){
        const int* e = J->stab + 4 * i;                     /* cache_off, nbytes, voff, cw */
        const unsigned char* src = J->cache + e[0]; unsigned char* dst = J->vt + e[2]; const int n = e[1];
        /* libc memcpy (measured: an HVX vmemu loop from DDR ran at ~1 GB/s on 4 threads, memcpy at 4.4 GB/s on one) */
        memcpy(dst, src, (unsigned)n);
    }
}
/* the scalar comparison path, compiled apart from the HVX kernels (measured: inlined together with them, the threaded
 * scalar path took 1.88 ms/frame instead of phase 2's 1.20) */
static void __attribute__((noinline)) pq_span_scalar_only(const pq_bufs* B, const int* sp, int base_off, int cw, int voff, int vbytes, int diag, pq_cnt* C){
    pq_span(B, sp, base_off, cw, voff, vbytes, 0, diag, C);
}
static void pq_band(pq_job* J, int k){
    if(J->phase == 1){ pq_stage(J, k); return; }
    if(J->phase == 2){
        pq_cnt C; C.npix = C.oob = C.oobd = C.tex_hvx = C.tex_fallback = C.z_hvx = C.z_scalar = 0;
        pq_bufs B; B.cache = J->inl ? J->vt : J->cache; B.cacheLen = J->cacheLen; B.vtcm = J->vt; B.fb = J->fb; B.fbLen = J->fbLen;
        B.zb = J->zb; B.zbLen = J->zbLen; B.cmds_end = J->cmds_end; B.gs = J->gs + 512 * k;
        const int tab_off = (int)(J->cmds_end - J->cmds);
        pq_pipe5 PP; pq_pipe5_init(&PP);
        const int j0 = J->bnd ? J->bnd[k] : (int)(((long long)J->nix * k) / J->T), j1 = J->bnd ? J->bnd[k + 1] : (int)(((long long)J->nix * (k + 1)) / J->T);
        const char* pf_next2 = 0; const char* pf_end = (const char*)J->cmds_end;
        if(j0 < j1){ const int p0 = J->sidx[2 * j0]; if(p0 >= 0 && p0 < tab_off) pf_next2 = (const char*)((uintptr_t)(J->cmds + p0) & ~(uintptr_t)4095); }
        for(int j = j0; j < j1; j++){
            if(J->recpf && pf_next2){
                const int cp = J->sidx[2 * j];
                if(cp >= 0 && cp < tab_off && (const char*)(J->cmds + cp) + 8192 >= pf_next2 && pf_next2 < pf_end){
                    Q6_l2fetch_AR((void*)pf_next2, (128u << 16) | (128u << 8) | 32u);
                    pf_next2 += 4096;
                }
            }
            const int pos = J->sidx[2 * j], si = J->sidx[2 * j + 1];
            if(pos < 6 || pos + 6 > tab_off || si < 0 || si >= J->nsurf || J->cmds[pos] != 2){ C.oobd++; continue; }
            const int count = J->cmds[pos + 3];
            if(count <= 0 || pos + 6 + 4 * ((count + 7) >> 3) > tab_off){ C.oobd++; continue; }
            const int* e = J->stab + 4 * si;
            pq_span_p5(&B, &PP, J->cmds + pos, e[0], e[3], e[2], e[1], J->diag, &C);
        }
        if(pq_pipe5_end(&B, &PP)){
            for(int j = j0; j < j1; j++){
                const int pos = J->sidx[2 * j], si = J->sidx[2 * j + 1];
                if(pos < 6 || pos + 6 > tab_off || si < 0 || si >= J->nsurf || J->cmds[pos] != 2) continue;
                const int count = J->cmds[pos + 3];
                if(count <= 0 || pos + 6 + 4 * ((count + 7) >> 3) > tab_off) continue;
                const int* e = J->stab + 4 * si;
                pq_span_tex_redo(&B, J->cmds + pos, e[0], e[3], e[2], e[1], &C);
            }
        }
        J->npix[k] = C.npix; J->oob[k] = C.oob; J->oobd[k] = C.oobd;
        J->thvx[k] = C.tex_hvx; J->tfb[k] = C.tex_fallback; J->zhvx[k] = C.z_hvx; J->zsc[k] = C.z_scalar;
        if(J->ntu > 0){                                    /* phase 6: this thread's share of the water */
            const unsigned long long w0 = HAP_perf_get_time_us();
            int p = 0, b = 0, h = 0, sc = 0;
            pq_turb_band3(J, k, &p, &b, &h, &sc);
            J->npix[k] += p; J->oobd[k] += b; J->tuh[k] = h; J->tus[k] = sc;
            J->tus_us[k] = (int)(HAP_perf_get_time_us() - w0);
        }
        return;
    }
    pq_cnt C; C.npix = C.oob = C.oobd = C.tex_hvx = C.tex_fallback = C.z_hvx = C.z_scalar = 0;
    pq_bufs B; B.cache = J->cache; B.cacheLen = J->cacheLen; B.vtcm = J->vtcm; B.fb = J->fb; B.fbLen = J->fbLen;
    B.zb = J->zb; B.zbLen = J->zbLen; B.cmds_end = J->cmds_end; B.gs = J->gs ? J->gs + 512 * k : 0;
    const int mode = (J->hvx && B.gs) ? 1 : 0;
    if(J->texpf && !mode && k < J->nix) pq_texpf(J, k);
    for(int j = k; j < J->nix; j += J->T){
        if(J->texpf && !mode && j + J->T < J->nix) pq_texpf(J, j + J->T);
        if(mode) pq_span(&B, J->cmds + J->ix[5 * j], J->ix[5 * j + 1], J->ix[5 * j + 2], J->ix[5 * j + 3], J->ix[5 * j + 4], 1, J->diag, &C);
        else pq_span_scalar_only(&B, J->cmds + J->ix[5 * j], J->ix[5 * j + 1], J->ix[5 * j + 2], J->ix[5 * j + 3], J->ix[5 * j + 4], J->diag, &C);
    }
    J->npix[k] = C.npix; J->oob[k] = C.oob; J->oobd[k] = C.oobd;
    J->thvx[k] = C.tex_hvx; J->tfb[k] = C.tex_fallback; J->zhvx[k] = C.z_hvx; J->zsc[k] = C.z_scalar;
}
#include "pq_turb.h"                                   /* phase 6: the water kernels */
static void pq_turb_band3(pq_job* J, int k, int* p, int* b, int* h, int* sc){
    pq_tctx X; X.tv = J->tv ? J->tv + (unsigned)k * PQ_TV : 0; X.staged = -1; X.active = 0; X.chunk = 0; X.n = 0; X.slot = 0; X.two = 0; X.dst = 0;
    for(int i = J->tb[k]; i < J->tb[k + 1]; i++){
        if(i + 1 < J->tb[k + 1]) pq_turb_pf(J->cmds, J->tab_off, J->tul[i + 1], J->fb, J->fbLen, J->zb, J->zbLen);
        if(X.tv && pq_turb_span_hvx3(J->cmds, J->tab_off, J->tul[i], J->fb, J->fbLen, J->zb, J->zbLen, &X, p)) (*h)++;
        else { pq_turb_span(J->cmds, J->tab_off, J->tul[i], J->fb, J->fbLen, J->zb, J->zbLen, p, b); (*sc)++; }
    }
    pq_tfinish(&X);
}
static int* g_ix = 0; static int g_ixcap = 0;
static pq_job g_jobmem;

static void pq_fork(pq_job* J, int T){
    g_job = J; g_cnt_forks++;
    for(int k = 1; k < T; k++) qurt_signal_set(&g_w[k - 1].go, 1u);
    pq_band(J, 0);
    for(int k = 1; k < T; k++){ qurt_signal_wait(&g_w[k - 1].done, 1u, QURT_SIGNAL_ATTR_WAIT_ANY); qurt_signal_clear(&g_w[k - 1].done, 1u); }
}
static int pq_spans_v2(pq_ctx* c, const int* cmds, int cmdsLen, const unsigned char* cache, int cacheLen,
                       unsigned char* fb, int fbLen, short* zb, int zbLen, int* status, int statusLen,
                       unsigned long long c0, unsigned long long t0){
    const int tab_off = cmds[3], nsurf = cmds[4], nspan = cmds[5];
    int bad = -1;
    const int mirror = (cmds[1] >> 15) & 1;
    int ndirty = mirror ? (cmdsLen >= 7 ? cmds[6] : -2) : 0;
    const int inl = mirror && ((cmds[1] >> 16) & 1), mcheck = (cmds[1] >> 17) & 1, dw = inl ? 3 : 2;
    if(mirror && ndirty < -1) bad = 9;
    if(bad < 0 && (tab_off < 6 + 2 * mirror || nsurf < 0 || nspan < 0 || (long long)tab_off + 4LL * nsurf + 2LL * nspan + (long long)dw * (ndirty > 0 ? ndirty : 0) > cmdsLen)) bad = 3;
    const int vrc = bad < 0 ? pq_vtcm_ensure(c) : -9;
    if(bad < 0 && (vrc != 0 || c->vtcm_bytes < 8192u)) bad = 4;
    const int* stab = cmds + tab_off; const int* sidx = stab + 4 * nsurf;
    unsigned vmax = 0;
    for(int i = 0; bad < 0 && i < nsurf; i++){             /* validate the host's table before anything is touched */
        const int* e = stab + 4 * i;
        if(e[0] < 0 || e[1] < 0 || e[1] > 65536 || (long long)e[0] + e[1] > cacheLen || e[2] < 0 || (mirror ? (e[2] != e[0] || (e[2] & 1)) : (e[2] & 127)) || e[3] < 0 || e[3] > 32767){ bad = 5; break; }
        const unsigned end = (unsigned)e[2] + (unsigned)e[1];
        if(end > vmax) vmax = end;
    }
    if(bad < 0 && vmax + 4096u > c->vtcm_bytes) bad = 6;
    if(bad < 0 && mirror && (unsigned)cacheLen + 4096u > c->vtcm_bytes) bad = 10;      /* the whole cache must fit */
    const int* dirty = sidx + 2 * nspan;
    for(int i = 0; bad < 0 && mirror && i < ndirty; i++)
        if(dirty[dw * i] < 0 || dirty[dw * i + 1] < 0 || (long long)dirty[dw * i] + dirty[dw * i + 1] > cacheLen
           || (inl && (dirty[3 * i + 2] < 6 || (long long)dirty[3 * i + 2] + (dirty[3 * i + 1] + 3) / 4 > cmdsLen))){ bad = 11; break; }
    int full = 0;
    const int* bnd = 0;                                    /* host's T+1 thread boundaries (mirror header) */
    if(bad < 0 && mirror && cmdsLen >= 8 && cmds[7] > 0){
        const int bo = cmds[7], Tb = (cmds[1] & 0xFF) > 4 ? 4 : (cmds[1] & 0xFF);
        if(Tb >= 1 && bo + Tb + 1 <= cmdsLen && cmds[bo] == 0 && cmds[bo + Tb] == nspan){
            bnd = cmds + bo;
            for(int k = 0; k < Tb; k++) if(bnd[k] > bnd[k + 1] || bnd[k] < 0){ bnd = 0; break; }
        }
    }
    int ntu = 0; const int* tul = 0;                       /* phase 6: the water spans */
    if(bad < 0 && mirror && ((cmds[1] >> 18) & 1)){
        const int Tt = (cmds[1] & 0xFF) > 4 ? 4 : (cmds[1] & 0xFF);
        const int to = (cmdsLen >= 8 && cmds[7] > 0) ? cmds[7] + Tt + 1 : -1;
        if(Tt < 1 || to < 8 || to >= cmdsLen) bad = 12;
        else { ntu = cmds[to]; tul = cmds + to + 1; if(ntu < 0 || (long long)to + 1 + ntu > cmdsLen){ bad = 12; ntu = 0; } }
    }
    if(bad < 0 && mirror && (!c->mirror_ok || ndirty < 0)){ ndirty = -1; full = 1; }
    int T = cmds[1] & 0xFF; if(T < 1) T = 1; if(T > 4) T = 4;
    pq_job* J = &g_jobmem;
    for(int k = 0; k < PQ_MAXT; k++){ J->npix[k] = J->oob[k] = J->oobd[k] = J->thvx[k] = J->tfb[k] = J->zhvx[k] = J->zsc[k] = J->tuh[k] = J->tus[k] = 0; }
    unsigned long long ts = 0, t2u = 0, t3u = 0; int tpix = 0, tbad = 0;
    if(bad < 0){
        if(T > 1) T = 1 + pq_pool_up(T - 1);
        J->cmds = cmds; J->T = T; J->diag = (cmds[1] >> 8) & 3; J->texpf = 0; J->hvx = 1;
        J->cache = cache; J->cacheLen = cacheLen; J->vtcm = c->vtcm; J->vt = c->vtcm; J->fb = fb; J->fbLen = fbLen; J->zb = zb; J->zbLen = zbLen;
        J->cmds_end = cmds + tab_off; J->gs = c->vtcm + c->vtcm_bytes - 4096u;
        J->stab = stab; J->sidx = sidx; J->nsurf = nsurf; J->nix = nspan; J->recpf = !((cmds[1] >> 14) & 1);
        J->mirror = mirror; J->inl = inl; J->ndirty = ndirty; J->dirty = dirty; J->bnd = (bnd && T == ((cmds[1] & 0xFF) > 4 ? 4 : (cmds[1] & 0xFF))) ? bnd : 0;
        J->phase = 1; pq_fork(J, T);
        if(mirror) c->mirror_ok = 1;
        if(mcheck && mirror){ int nd = 0, first = -1; for(int i = 0; i < cacheLen; i++) if(c->vtcm[i] != cache[i]){ if(first < 0) first = i; nd++; }
                              if(statusLen > 21){ status[20] = nd; status[21] = first; } }
        ts = HAP_perf_get_time_us();
        J->ntu = 0;
        if(ntu > 0){                                       /* phase 3: the water, split over the threads by pixels */
            long long tot = 0;
            for(int i = 0; i < ntu; i++){ const int p = tul[i]; const int n = (p >= 8 && p + 4 <= tab_off) ? cmds[p + 3] : 0; tot += (n > 0 && n < 65536 ? n : 0) + 32; }
            long long acc = 0; int i = 0; J->tb[0] = 0;
            for(int k = 1; k < T; k++){
                const long long target = (tot * k) / T;
                while(i < ntu){ const int p = tul[i]; const int n = (p >= 8 && p + 4 <= tab_off) ? cmds[p + 3] : 0; const long long cst = (n > 0 && n < 65536 ? n : 0) + 32;
                                if(acc + cst > target) break; acc += cst; i++; }
                J->tb[k] = i;
            }
            J->tb[T] = ntu; J->tul = tul; J->ntu = ntu; J->tab_off = tab_off;
            J->tv = (((cmds[1] >> 19) & 1) && (unsigned)cacheLen + 4096u + (unsigned)PQ_MAXT * PQ_TV <= c->vtcm_bytes)
                    ? c->vtcm + c->vtcm_bytes - 4096u - (unsigned)PQ_MAXT * PQ_TV : 0;
        }
        for(int k = 0; k < PQ_MAXT; k++) J->tus_us[k] = 0;
        J->phase = 2; pq_fork(J, T);
        t2u = HAP_perf_get_time_us();
        { int mx = 0; for(int k = 0; k < PQ_MAXT; k++) if(J->tus_us[k] > mx) mx = J->tus_us[k]; t3u = (unsigned long long)mx; }
        J->phase = 0; J->ntu = 0;
    }
    int npix = tpix, oob = 0, oobd = tbad, a = 0, b2 = 0, d = 0, e2 = 0;
    for(int k = 0; k < PQ_MAXT; k++){ npix += J->npix[k]; oob += J->oob[k]; oobd += J->oobd[k]; a += J->thvx[k]; b2 += J->tfb[k]; d += J->zhvx[k]; e2 += J->zsc[k]; }
    status[0] = 0x5A; status[1] = bad < 0 ? nspan : 0; status[2] = npix;
    status[3] = (int)(HAP_perf_get_pcycles() - c0); status[4] = (int)(HAP_perf_get_time_us() - t0);
    status[5] = oob; status[6] = bad < 0 ? -1 : bad; status[7] = nsurf; status[8] = oobd;
    if(statusLen > 9) status[9] = T;
    if(statusLen > 12){ status[10] = (int)vmax; status[11] = nsurf; status[12] = c->acq_code; }
    if(statusLen > 16){ status[13] = a; status[14] = b2; status[15] = d; status[16] = e2; }
    if(statusLen > 17) status[17] = 1 | (c->hvx_rc << 4);
    if(statusLen > 18) status[18] = ts ? (int)(ts - t0) : -1;     /* us until the staging was done */
    if(statusLen > 19) status[19] = mirror ? (full ? -1 : ndirty) : -2;   /* dirty ranges copied, -1 = whole cache */
    if(statusLen > 23){ int th = 0, tsc = 0; for(int k = 0; k < PQ_MAXT; k++){ th += J->tuh[k]; tsc += J->tus[k]; } status[22] = th; status[23] = tsc; }
    if(statusLen > 25){ status[24] = (ts && t2u) ? (int)(t2u - ts) : -1; status[25] = ntu > 0 ? (int)t3u : -1; }
    return AEE_SUCCESS;
}

int pxquake_spans(remote_handle64 h, const int* cmds, int cmdsLen, const unsigned char* cache, int cacheLen,
                  unsigned char* fb, int fbLen, short* zb, int zbLen, int* status, int statusLen){
    pq_ctx* c = pq_get(h);
    if(!c || statusLen < 9) return AEE_EBADPARM;
    pq_vote(c);
    c->calls++;
    const unsigned long long c0 = HAP_perf_get_pcycles(), t0 = HAP_perf_get_time_us();
    if(cmdsLen >= 6 && cmds[0] == 3 && ((cmds[1] >> 13) & 1) && ((cmds[1] >> 12) & 1) && cmds[2] == 7)
        return pq_spans_v2(c, cmds, cmdsLen, cache, cacheLen, fb, fbLen, zb, zbLen, status, statusLen, c0, t0);
    if(cmdsLen >= 2 && cmds[0] == 3 && cmds[1] >= 1){
        /* pass 1: index */
        const int diag = (cmds[1] >> 8) & 3;
        const int hvx = (cmds[1] >> 12) & 1;
        int T = (cmds[1] & 0xFF) > PQ_MAXT ? PQ_MAXT : (cmds[1] & 0xFF);
        if(hvx && T > 4) T = 4;
        if(T < 1) T = 1;
        int need = 5 * (cmdsLen / 10 + 1);
        if(need > g_ixcap){ if(g_ix) free(g_ix); g_ix = (int*)malloc(sizeof(int) * (unsigned)need); g_ixcap = g_ix ? need : 0; }
        if(!g_ix){ status[0] = -2; return AEE_SUCCESS; }
        int nix = 0, badop = -1, nsurf = 0, base_off = 0, cw = 0, i = 2, voff = -1, vbytes = 0, vused = 0, nstaged = 0, vrc = 1;
        /* the stream is cold in the DSP's caches (FastRPC invalidated it): walking span headers one by one costs a
         * DDR miss each. Prefetch it into L2 page by page, 8 KB ahead of the walk (an l2fetch stops at a page
         * boundary; a new one replaces the running one, so they are issued one page at a time). */
        const int pf_on = !((cmds[1] >> 10) & 1);
        const char* pf_end = (const char*)(cmds + cmdsLen);
        const char* pf_next = (const char*)((uintptr_t)(cmds + 2) & ~(uintptr_t)4095);
        while(i < cmdsLen){
            if(pf_on && (const char*)(cmds + i) + 8192 >= pf_next && pf_next < pf_end){
                Q6_l2fetch_AR((void*)pf_next, (128u << 16) | (128u << 8) | 32u);
                pf_next += 4096;
            }
            const int op = cmds[i];
            if(op == 0) break;
            if(op == 1){ if(i + 3 > cmdsLen){ badop = i; break; } base_off = cmds[i + 1]; cw = cmds[i + 2]; voff = -1; vbytes = 0; nsurf++; i += 3; continue; }
            if(op == 4){   /* SURF + stage: [4, cache_off, cachewidth, nbytes of the block's data] */
                if(i + 4 > cmdsLen){ badop = i; break; }
                base_off = cmds[i + 1]; cw = cmds[i + 2]; vbytes = cmds[i + 3]; voff = -1; nsurf++; i += 4;
                if(vrc == 1) vrc = pq_vtcm_ensure(c);
                const unsigned al = ((unsigned)vused + 127u) & ~127u;
                if(vrc == 0 && vbytes > 0 && base_off >= 0 && base_off + vbytes <= cacheLen && al + (unsigned)vbytes + 4096u <= c->vtcm_bytes){
                    memcpy(c->vtcm + al, cache + base_off, (unsigned)vbytes); c->mirror_ok = 0; voff = (int)al; vused = (int)(al + (unsigned)vbytes); nstaged++;
                } else vbytes = 0;
                continue;
            }
            if(op != 2 || i + 6 > cmdsLen){ badop = i; break; }
            const int count = cmds[i + 3], nseg = (count + 7) >> 3;
            if(count <= 0 || i + 6 + 4 * nseg > cmdsLen || 5 * nix + 5 > g_ixcap){ badop = i; break; }
            g_ix[5 * nix] = i; g_ix[5 * nix + 1] = base_off; g_ix[5 * nix + 2] = cw; g_ix[5 * nix + 3] = voff; g_ix[5 * nix + 4] = vbytes; nix++;
            i += 6 + 4 * nseg;
        }
        /* pass 2: draw */
        if(T > 1) T = 1 + pq_pool_up(T - 1);
        pq_job* J = &g_jobmem;
        J->cmds = cmds; J->ix = g_ix; J->nix = nix; J->T = T; J->diag = diag; J->texpf = !((cmds[1] >> 11) & 1);
        J->hvx = hvx; J->cmds_end = cmds + cmdsLen; J->phase = 0;
        J->gs = (hvx && c->vtcm && c->vtcm_bytes >= 8192u) ? c->vtcm + c->vtcm_bytes - 4096u : 0;
        J->cache = cache; J->cacheLen = cacheLen; J->vtcm = c->vtcm; J->fb = fb; J->fbLen = fbLen; J->zb = zb; J->zbLen = zbLen;
        g_job = J;
        for(int k = 1; k < T; k++) qurt_signal_set(&g_w[k - 1].go, 1u);
        pq_band(J, 0);
        for(int k = 1; k < T; k++){ qurt_signal_wait(&g_w[k - 1].done, 1u, QURT_SIGNAL_ATTR_WAIT_ANY); qurt_signal_clear(&g_w[k - 1].done, 1u); }
        int npix = 0, oob = 0, oobd = 0;
        for(int k = 0; k < T; k++){ npix += J->npix[k]; oob += J->oob[k]; oobd += J->oobd[k]; }
        status[0] = 0x5A; status[1] = nix; status[2] = npix;
        status[3] = (int)(HAP_perf_get_pcycles() - c0); status[4] = (int)(HAP_perf_get_time_us() - t0);
        status[5] = oob; status[6] = badop; status[7] = nsurf; status[8] = oobd;
        if(statusLen > 9) status[9] = T;
        if(statusLen > 12){ status[10] = vused; status[11] = nstaged; status[12] = vrc == 1 ? 0 : (vrc == 0 ? c->acq_code : vrc); }
        if(statusLen > 16){ int a = 0, b = 0, d = 0, e = 0; for(int k = 0; k < T; k++){ a += J->thvx[k]; b += J->tfb[k]; d += J->zhvx[k]; e += J->zsc[k]; }
                            status[13] = a; status[14] = b; status[15] = d; status[16] = e; }
        if(statusLen > 17) status[17] = (J->gs ? 1 : 0) | (c->hvx_rc << 4);
        return AEE_SUCCESS;
    }
    int nspans = 0, npix = 0, oob = 0, badop = -1, nsurf = 0, oobd = 0;
    const unsigned char* pbase = cache; int cw = 0; int base_off = 0;
    int i = 0;
    while(i < cmdsLen){
        const int op = cmds[i];
        if(op == 0) break;
        if(op == 3){ i += 2; continue; }   /* config: nthreads 0 = this serial path */
        if(op == 1){
            if(i + 3 > cmdsLen){ badop = i; break; }
            base_off = cmds[i + 1]; cw = cmds[i + 2]; pbase = cache + base_off; nsurf++;
            i += 3; continue;
        }
        if(op != 2 || i + 6 > cmdsLen){ badop = i; break; }
        const int fb_off = cmds[i + 1], z_off = cmds[i + 2], count = cmds[i + 3];
        unsigned izi = (unsigned)cmds[i + 4]; const unsigned izistep = (unsigned)cmds[i + 5];
        const int nseg = (count + 7) >> 3;
        i += 6;
        if(count <= 0 || i + 4 * nseg > cmdsLen){ badop = i - 6; break; }
        const int* seg = cmds + i;
        i += 4 * nseg;
        nspans++;
        if(fb_off < 0 || fb_off + count > fbLen || z_off < 0 || z_off + count > zbLen){ oobd++; continue; }
        /* texture */
        unsigned char* pdest = fb + fb_off;
        int left = count;
        for(int k = 0; k < nseg; k++){
            int n = left >= 8 ? 8 : left; left -= n;
            int s = seg[4 * k], t = seg[4 * k + 1]; const int ss = seg[4 * k + 2], ts = seg[4 * k + 3];
            do {
                const int idx = (s >> 16) + (t >> 16) * cw;
                if((unsigned)(base_off + idx) < (unsigned)cacheLen) *pdest = pbase[idx]; else oob++;
                pdest++;
                s += ss; t += ts;
            } while(--n > 0);
        }
        npix += count;
        /* z, exactly id's D_DrawZSpans */
        short* pz = zb + z_off;
        int cnt = count;
        if((uintptr_t)pz & 0x02){ *pz++ = (short)((int)izi >> 16); izi += izistep; cnt--; }
        int dc = cnt >> 1;
        if(dc > 0){
            do {
                unsigned lt = (unsigned)((int)izi >> 16);
                izi += izistep;
                lt |= izi & 0xFFFF0000u;
                izi += izistep;
                *(unsigned*)pz = lt;
                pz += 2;
            } while(--dc > 0);
        }
        if(cnt & 1) *pz = (short)((int)izi >> 16);
    }
    status[0] = 0x5A; status[1] = nspans; status[2] = npix;
    status[3] = (int)(HAP_perf_get_pcycles() - c0); status[4] = (int)(HAP_perf_get_time_us() - t0);
    status[5] = oob; status[6] = badop; status[7] = nsurf; status[8] = oobd;
    return AEE_SUCCESS;
}

/* phase 5: the persistent frame worker (dspqueue) */
#include "pq_queue.h"
#include "pq_mbox.h"
