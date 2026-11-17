/* pc_dsp.c - host bridge of PulseX Quake phase 2: one FastRPC session to libpxquake_skel.so (PulseX/skel/pxquake),
 * shared rpcmem buffers, the per-frame span queue and the call-cost bench. API: pc_dsp.h.
 * Session handling copied from living/dsp/pulse_living.cpp (measured there): libcdsprpc from the driver store,
 * ADSP_LIBRARY_PATH = the folder of the exe (the skel + its signed .cat lie there), unsigned PD, retry 0x80000406;
 * close = release on the DSP -> unregister + free buffers -> FASTRPC_SESSION_CLOSE -> handle close (44 = proof).
 * pcdsp_close is idempotent and is called from VID_Shutdown (every exit path of the port: Sys_Quit, Sys_Error,
 * PC_HarnessExit) and, as last resort, from atexit (this exe uses the shared CRT, so exit() runs it while FastRPC works). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <psapi.h>	/* phase 9: K32GetProcessMemoryInfo */
#include "remote.h"
#include "pxquake.h"
#include "dspqueue.h"
#include "pc_dsp.h"

typedef int   (*fn_open)(const char*, remote_handle64*);
typedef int   (*fn_close)(remote_handle64);
typedef int   (*fn_invoke)(remote_handle64, uint32_t, remote_arg*);
typedef int   (*fn_sess)(uint32_t, void*, uint32_t);
typedef void  (*fn_rinit)(void);
typedef void* (*fn_ralloc)(int, unsigned, int);
typedef int   (*fn_rtofd)(void*);
typedef void  (*fn_regbuf)(void*, int, int);
typedef void  (*fn_rfree)(void*);
static fn_open p_open; static fn_close p_close; static fn_invoke p_invoke; static fn_sess p_sess;
static fn_ralloc p_ralloc; static fn_rtofd p_rtofd; static fn_regbuf p_regbuf; static fn_rfree p_rfree;

static remote_handle64 H = 0;
static volatile LONG g_open = 0;
static char g_err[512] = "";

static void dlog(const char* fmt, ...){
    va_list ap; va_start(ap, fmt); fputs("[pcdsp] ", stderr); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr); fflush(stderr);
}
static void seterr(const char* fmt, ...){
    va_list ap; va_start(ap, fmt); vsnprintf(g_err, sizeof g_err, fmt, ap); va_end(ap);
    dlog("ERROR: %s", g_err);
}
const char* pcdsp_error(void){ return g_err; }

/* phase 7: witnesses. Every RPC is checked: made inside a window-message handler, or from another thread than the one
 * that opened the session? Both must stay 0; both are printed at the close and fail the run (exit code 9). */
static _Thread_local int t_msg_depth = 0;
static volatile LONG g_rpc_in_msg = 0, g_rpc_foreign = 0; static volatile DWORD g_owner_tid = 0;
void pcdsp_msg_enter(void){ t_msg_depth++; }
void pcdsp_msg_leave(void){ t_msg_depth--; }
static void rpc_check(const char* what, unsigned m){
    if(t_msg_depth > 0){ InterlockedIncrement(&g_rpc_in_msg); dlog("ERROR: DSP RPC %s (method %u) made INSIDE a window-message handler", what, m); }
    if(g_owner_tid && GetCurrentThreadId() != g_owner_tid){ InterlockedIncrement(&g_rpc_foreign); dlog("ERROR: DSP RPC %s (method %u) from thread %lu, the session belongs to %lu", what, m, GetCurrentThreadId(), (unsigned long)g_owner_tid); }
}
/* the qaic stub calls these; libcdsprpc is bound at run time, so they forward */
int remote_handle64_open(const char* n, remote_handle64* h){ rpc_check("open", 0); return p_open ? p_open(n, h) : -1; }
int remote_handle64_close(remote_handle64 h){ rpc_check("close", 0); return p_close ? p_close(h) : -1; }
int remote_handle64_invoke(remote_handle64 h, uint32_t s, remote_arg* a){
    rpc_check("invoke", (unsigned)REMOTE_SCALARS_METHOD(s));
    int rc = p_invoke ? p_invoke(h, s, a) : -1;
    if(rc != 0) dlog("rpc rc=%d (0x%08x)%s method=%u", rc, (unsigned)rc, (((unsigned)rc & 0xFFFFFC00u) == 0x80000400u) ? " [DSP-side]" : "",
                     (unsigned)REMOTE_SCALARS_METHOD(s));
    return rc;
}

static LARGE_INTEGER qf;
static double now_ms(void){ LARGE_INTEGER c; if(!qf.QuadPart) QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&c); return 1000.0 * (double)c.QuadPart / (double)qf.QuadPart; }
/* phase 7: the shutdown - ordered, bounded, logged; a watchdog that only reports */
static int g_closing = 0, g_shut_timeouts = 0, g_dialog = 1; static double g_wait_lim_ms = 2000.0;   /* phase 9: 2 s (was 5 s) */
/* phase 9: a session whose DSP stopped answering is LOST: no further jobs, waits or RPCs; the game renders on the CPU */
static int g_dsp_lost = 0, g_test_lost_frame = -1, g_test_lost_noack = 0;
int pcdsp_lost(void){ return g_dsp_lost; }
void pcdsp_set_test_lost(int frame, int noack){ g_test_lost_frame = frame; g_test_lost_noack = noack ? 1 : 0; }
static void dsp_mark_lost(const char* why);
static int g_recycle_ms_set = 60000; void pcdsp_set_recycle(int ms){ g_recycle_ms_set = ms < 0 ? 0 : ms; }   /* phase 9: -dsprecycle */
static long g_recycles = 0, g_recycle_bad = 0; static double g_mb_t0 = 0.0, g_recycle_ms = 0.0, g_recycle_max_ms = 0.0;
static int g_rc_acq = 0, g_rc_wj = 0, g_rc_wc = 0;
static int lost_stop_acked(void);                   /* phase 9: the stop flag only, 300 ms for the ack (no RPC) */
static const char* volatile g_shut_phase = "-"; static double g_shut_t0 = 0.0; static volatile LONG g_shut_done = 0;
void pcdsp_set_dialog(int on){ g_dialog = on ? 1 : 0; }
static void shut_phase(const char* p){ g_shut_phase = p; printf("PCDSP shutdown t=%.1f ms phase=%s\n", now_ms() - g_shut_t0, p); fflush(stdout); }
static void dsp_log_line(const char* why);           /* phase 7: the DSP idle log from shared memory (no RPC) */
static DWORD WINAPI shut_watch(void* unused){
    (void)unused; int shown = 0; double next = 4000.0;
    while(!g_shut_done){
        Sleep(100);
        const double t = now_ms() - g_shut_t0;
        if(g_shut_done || t < next) continue;
        next += 5000.0;
        printf("PCDSP SHUTDOWN STUCK %.1f s in phase %s: the DSP session is still open - do NOT kill the process, restart the computer before the next NPU job\n", t / 1000.0, (const char*)g_shut_phase);
        fflush(stdout);
        fprintf(stderr, "[pcdsp] SHUTDOWN STUCK %.1f s in phase %s\n", t / 1000.0, (const char*)g_shut_phase); fflush(stderr);
        dsp_log_line("stuck"); Sleep(200); dsp_log_line("stuck+200ms");
        if(g_dialog && !shown){ shown = 1;
            MessageBoxW(NULL, L"PulseX Quake kan inte st\u00e4nga NPU-sessionen: avst\u00e4ngningen h\u00e4nger.\n\n"
                              L"Avsluta INTE processen i Aktivitetshanteraren. Starta om datorn innan n\u00e4sta NPU-jobb.\n"
                              L"Loggen: bin\\pulsex-quake.log", L"PulseX Quake", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL | MB_SETFOREGROUND); }
    }
    return 0;
}

static char g_dir[MAX_PATH] = "";
static int set_skel_path(void){
    char dir[MAX_PATH] = "", f[MAX_PATH + 40];
    if(!GetModuleFileNameA(NULL, dir, MAX_PATH)){ seterr("GetModuleFileName failed"); return 2; }
    char* sl = strrchr(dir, '\\'); if(sl) *sl = 0;
    strncpy(g_dir, dir, MAX_PATH - 1);
    snprintf(f, sizeof f, "%s\\libpxquake_skel.so", dir);
    if(GetFileAttributesA(f) == INVALID_FILE_ATTRIBUTES){ seterr("libpxquake_skel.so is not next to the exe (%s)", dir); return 2; }
    snprintf(f, sizeof f, "%s\\libpxquake_skel.cat", dir);
    if(GetFileAttributesA(f) == INVALID_FILE_ATTRIBUTES){ seterr("libpxquake_skel.cat (signed catalog) is not next to the exe (%s)", dir); return 2; }
    for(const char* c = dir; *c; ++c) if((unsigned char)*c >= 128){ seterr("exe folder has a non-ASCII character (%s)", dir); return 2; }
    if(GetModuleHandleA("libcdsprpc.dll")) dlog("WARNING: libcdsprpc.dll already loaded before ADSP_LIBRARY_PATH was set");
    SetEnvironmentVariableA("ADSP_LIBRARY_PATH", dir);
    _putenv_s("ADSP_LIBRARY_PATH", dir);
    const char* seen = NULL;
    HMODULE u = LoadLibraryA("ucrtbase.dll");
    if(u){
        typedef int (*fn_putenv)(const char*, const char*); typedef char* (*fn_getenv)(const char*);
        fn_putenv pe = (fn_putenv)GetProcAddress(u, "_putenv_s"); fn_getenv ge = (fn_getenv)GetProcAddress(u, "getenv");
        if(pe) pe("ADSP_LIBRARY_PATH", dir);
        if(ge) seen = ge("ADSP_LIBRARY_PATH");
    }
    if(!seen) seen = getenv("ADSP_LIBRARY_PATH");
    if(!seen || _stricmp(seen, dir) != 0){ seterr("ADSP_LIBRARY_PATH did not take: wanted %s", dir); return 2; }
    dlog("ADSP_LIBRARY_PATH = %s", seen);
    return 0;
}
static HMODULE g_cdsprpc;
static HMODULE load_cdsprpc(void){
    HMODULE h = NULL;
    const char* ROOT = "C:\\Windows\\System32\\DriverStore\\FileRepository\\";
    const char* pats[2] = { "*qcnspmcdm*", "qc*" };
    for(int p = 0; p < 2 && !h; p++){
        char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s%s", ROOT, pats[p]);
        WIN32_FIND_DATAA fd; HANDLE fh = FindFirstFileA(pat, &fd);
        if(fh == INVALID_HANDLE_VALUE) continue;
        do {
            if(!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            char cand[MAX_PATH]; snprintf(cand, sizeof cand, "%s%s\\libcdsprpc.dll", ROOT, fd.cFileName);
            if(GetFileAttributesA(cand) == INVALID_FILE_ATTRIBUTES) continue;
            h = LoadLibraryA(cand);
            if(h){ dlog("libcdsprpc: %s", fd.cFileName); break; }
        } while(FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    if(!h) h = LoadLibraryA("libcdsprpc.dll");
    return h;
}

/* ---------------------------------------------------------------- shared buffers */
#define NB 16
static struct { void* p; int n; int fd; int mapped; } g_b[NB];
/* phase 5: dspqueue + persistent mappings */
typedef int (*fn_dqc)(int, uint32_t, uint32_t, uint32_t, dspqueue_callback_t, dspqueue_callback_t, void*, dspqueue_t*);
typedef int (*fn_dqx)(dspqueue_t, uint64_t*);
typedef int (*fn_dqw)(dspqueue_t, uint32_t, uint32_t, struct dspqueue_buffer*, uint32_t, const uint8_t*, uint32_t);
typedef int (*fn_dqr)(dspqueue_t, uint32_t*, uint32_t, uint32_t*, struct dspqueue_buffer*, uint32_t, uint32_t*, uint8_t*, uint32_t);
typedef int (*fn_dqcl)(dspqueue_t);
typedef int (*fn_fmm)(int, int, void*, int, size_t, enum fastrpc_map_flags);
typedef int (*fn_fmu)(int, int, void*, size_t);
static fn_dqc p_dqc; static fn_dqx p_dqx; static fn_dqw p_dqw; static fn_dqr p_dqr; static fn_dqcl p_dqcl; static fn_fmm p_fmm; static fn_fmu p_fmu;
static int g_dq = 0, g_dq_noflush = 0; static dspqueue_t g_dqq = NULL; static unsigned g_dqseq = 0;
typedef struct { uint32_t type, seq; int cmdsLen, cacheLen, fbLen, zbLen; uint32_t flags; uint32_t pad; } pq_qreq;
typedef struct { uint32_t type, seq; int st[24]; } pq_qrsp;
static int buf_index(const void* p){ for(int i = 0; i < NB; i++) if(g_b[i].p && (const char*)p >= (const char*)g_b[i].p && (const char*)p < (const char*)g_b[i].p + g_b[i].n) return i; return -1; }
static void dq_ref(struct dspqueue_buffer* b, const void* p, int bytes, uint32_t flags){
    const int i = buf_index(p); memset(b, 0, sizeof *b);
    if(i < 0) return;
    b->fd = (uint32_t)g_b[i].fd; b->offset = (uint32_t)((const char*)p - (const char*)g_b[i].p);
    b->size = (uint32_t)((bytes + 127) & ~127); if(b->offset + b->size > (uint32_t)g_b[i].n) b->size = (uint32_t)g_b[i].n - b->offset;
    b->flags = g_dq_noflush ? 0 : flags; b->ptr = (void*)p;
}

void* pcdsp_alloc(int bytes){
    if(!p_ralloc){ seterr("pcdsp_alloc before pcdsp_open"); return NULL; }
    int n = ((bytes + 4095) / 4096) * 4096;
    for(int i = 0; i < NB; i++) if(!g_b[i].p){
        void* p = p_ralloc(25 /* system heap */, 1 /* default flags */, n);
        if(!p){ seterr("rpcmem_alloc(%d) failed", n); return NULL; }
        if(p_regbuf && p_rtofd) p_regbuf(p, n, p_rtofd(p));
        memset(p, 0, (size_t)n);
        g_b[i].p = p; g_b[i].n = n; g_b[i].fd = p_rtofd ? p_rtofd(p) : -1; g_b[i].mapped = 0;
        if(g_dq && p_fmm && g_b[i].fd >= 0){   /* phase 5: mapped once for the queue */
            int mr = p_fmm(CDSP_DOMAIN_ID, g_b[i].fd, p, 0, (size_t)n, FASTRPC_MAP_FD);
            if(mr == 0) g_b[i].mapped = 1; else dlog("fastrpc_mmap(fd=%d, %d B) failed rc=0x%x", g_b[i].fd, n, (unsigned)mr);
        }
        return p;
    }
    seterr("out of buffer slots"); return NULL;
}
static void free_all(void){
    for(int i = 0; i < NB; i++) if(g_b[i].p){
        if(g_b[i].mapped && p_fmu){ p_fmu(CDSP_DOMAIN_ID, g_b[i].fd, g_b[i].p, (size_t)g_b[i].n); g_b[i].mapped = 0; }
        if(p_regbuf && p_rtofd) p_regbuf(g_b[i].p, g_b[i].n, -1);
        if(p_rfree) p_rfree(g_b[i].p);
        g_b[i].p = NULL; g_b[i].n = 0;
    }
}

/* ---------------------------------------------------------------- session */
static void close_tag(const char* tag);
static void close_any(const char* tag);
static int g_join_bad = 0;                          /* a DSP thread did not end cleanly: the run exits 7 */
static int g_lpm_bad;
static int g_lost_exit = 0;                         /* phase 9: the DSP was lost during the session */
int pcdsp_exit_code(void){ return (g_rpc_in_msg || g_rpc_foreign) ? 9 : g_join_bad ? 7 : g_lpm_bad ? 10 : g_lost_exit ? 11 : g_shut_timeouts ? 8 : 0; }
static void at_exit(void){ close_any("atexit"); }
int pcdsp_is_open(void){ return g_open ? 1 : 0; }
/* ---- the worker (pipeline mode only) */
static int g_pipe = 0;
static HANDLE g_wthr, g_wgo, g_wdone; static volatile int g_wquit;
static void (*volatile g_wfn)(void*); static void* volatile g_warg; static volatile int g_wbusy;
static DWORD WINAPI worker_main(void* unused){
    (void)unused;
    for(;;){
        WaitForSingleObject(g_wgo, INFINITE);
        if(g_wquit){ SetEvent(g_wdone); return 0; }
        g_wfn(g_warg);
        SetEvent(g_wdone);
    }
}
static void w_submit(void (*fn)(void*), void* arg){ g_wfn = fn; g_warg = arg; g_wbusy = 1; SetEvent(g_wgo); }
static void w_wait(void){ if(g_wbusy){ WaitForSingleObject(g_wdone, INFINITE); g_wbusy = 0; } }
static void w_run(void (*fn)(void*), void* arg){ w_wait(); w_submit(fn, arg); w_wait(); }
static int open_direct(void);
static int g_open_rc;
static void w_open(void* a){ (void)a; g_open_rc = open_direct(); }
void pcdsp_set_pipe(int on){ g_pipe = on ? 1 : 0; }
int pcdsp_pipe(void){ return g_pipe; }
static int dq_setup(void);
void pcdsp_set_dq(int on){ g_dq = on ? 1 : 0; if(g_dq) g_pipe = 1; }
void pcdsp_set_dq_noflush(int on){ g_dq_noflush = on ? 1 : 0; }
int pcdsp_open(void){
    if(g_open) return 0;
    if(g_dq){ int rc = open_direct(); if(rc) return rc; return dq_setup(); }
    if(!g_pipe) return open_direct();
    if(!g_wthr){
        g_wgo = CreateEventA(NULL, FALSE, FALSE, NULL); g_wdone = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_wthr = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
        if(!g_wthr || !g_wgo || !g_wdone){ seterr("could not start the DSP worker thread"); return 3; }
    }
    w_run(w_open, NULL);
    return g_open_rc;
}
static int open_direct(void){
    if(g_open) return 0;
    static HMODULE lib = NULL;
    if(!lib){
        int rc = set_skel_path(); if(rc) return rc;
        lib = load_cdsprpc();
        g_cdsprpc = lib;
        if(!lib){ seterr("libcdsprpc.dll not found"); return 1; }
        p_open = (fn_open)GetProcAddress(lib, "remote_handle64_open"); p_close = (fn_close)GetProcAddress(lib, "remote_handle64_close");
        p_invoke = (fn_invoke)GetProcAddress(lib, "remote_handle64_invoke"); p_sess = (fn_sess)GetProcAddress(lib, "remote_session_control");
        p_ralloc = (fn_ralloc)GetProcAddress(lib, "rpcmem_alloc"); p_rtofd = (fn_rtofd)GetProcAddress(lib, "rpcmem_to_fd");
        p_regbuf = (fn_regbuf)GetProcAddress(lib, "remote_register_buf"); p_rfree = (fn_rfree)GetProcAddress(lib, "rpcmem_free");
        fn_rinit ri = (fn_rinit)GetProcAddress(lib, "rpcmem_init");
        if(!p_open || !p_close || !p_invoke || !p_sess || !p_ralloc || !p_rfree){ seterr("libcdsprpc.dll lacks an expected export"); return 1; }
        if(ri) ri();
        atexit(at_exit);
    }
    int rc = 0;
    for(int attempt = 0; attempt < 6; attempt++){
        struct remote_rpc_control_unsigned_module um; um.domain = CDSP_DOMAIN_ID; um.enable = 1;
        int uc = p_sess(DSPRPC_CONTROL_UNSIGNED_MODULE, &um, sizeof um);
        if(uc != 0){ seterr("DSPRPC_CONTROL_UNSIGNED_MODULE refused: rc=%d", uc); return 10000 + uc; }
        H = 0;
        rc = pxquake_open(pxquake_URI "&_dom=cdsp", &H);
        if(rc == 0) break;
        dlog("open attempt %d: rc=%d (0x%08x)", attempt + 1, rc, (unsigned)rc);
        if((unsigned)rc != 0x80000406u) break;
        Sleep(700);
    }
    if(rc != 0){ seterr("pxquake_open failed rc=%d (0x%08x), skel folder %s", rc, (unsigned)rc, g_dir); H = 0; return rc; }
    InterlockedExchange(&g_open, 1);
    g_owner_tid = GetCurrentThreadId();                 /* phase 7: every later RPC must come from this thread */
    dlog("session open: handle=0x%llx skel=%s\\libpxquake_skel.so", (unsigned long long)H, g_dir);
    return 0;
}

extern int pc_inplace_same, pc_inplace_diff;   /* id/d_surf.c */
static int g_vtcm; static long g_staged_bytes, g_staged_surfs; static int g_vtcm_code;   /* defined with the queue below */
static int g_hvx, g_threads, g_gsok; static long g_thvx, g_tfb, g_zhvx, g_zsc;               /* ditto */
static double g_stage_us; static long g_stage_calls; static int g_mirror; static long g_dirty_bytes, g_full_copies;
static long g_sync_full;
static long g_async_partial; static int g_nsb;   /* defined with the queue below */
static long g_async_jobs, g_sync_jobs, g_waits, g_async_full, g_inline_bytes, g_mcheck_calls, g_mcheck_bad; static int g_mcheck_first = -1; static double g_wait_ms;
static float g_frame_wait[20000]; static int g_nfw;
static float g_lat[20000], g_dur[20000]; static int g_nlat;
/* frame statistics (printed at close) */
static long g_frames, g_calls, g_guard_flushes, g_full_flushes, g_errors, g_spans_total, g_pix_total;
/* phase 6: the water. Records: [6, 0, turb[128], tex[1024 words]] once per stream and texture; [5, fb_off, z_off, count,
 * izi, izistep, texpos, 4 ints per 16-pixel segment]; the list of op-5 positions follows the thread boundaries. */
#define TU_MAX 65536
#define TU_TEXW (2 + 128 + 1024)
static int g_turb_hvx = 1; static long g_turb_hvx_spans, g_turb_sc_spans; static double g_ph2_us, g_ph3_us; static long g_ph_jobs, g_ph3_jobs;
static int g_turb = 1, g_turb_sick = 0, g_tu[TU_MAX], g_ntu = 0, g_turb_texpos = -1;
static const unsigned char* g_turb_tex; static const int* g_turb_tab;
static long g_turb_spans, g_turb_pix, g_turb_texrecs;
static double g_rpc_ms_total, g_dsp_us_total, g_frame_rpc_ms, g_frame_dsp_us; static int g_frame_calls;
#define MAXF 20000
static float g_frame_ms[MAXF], g_frame_us[MAXF]; static int g_nf;

static int cmpf(const void* a, const void* b){ float x = *(const float*)a, y = *(const float*)b; return x < y ? -1 : x > y; }
static void stat_line(const char* name, float* v, int n){
    if(n <= 0){ printf("PCDSP %s n=0\n", name); return; }
    qsort(v, (size_t)n, sizeof(float), cmpf);
    double s = 0; for(int i = 0; i < n; i++) s += v[i];
    int i99 = (int)(0.99 * (n - 1) + 0.5);
    printf("PCDSP %s n=%d median=%.4f p99=%.4f mean=%.4f min=%.4f max=%.4f\n", name, n, v[n / 2], v[i99], s / n, v[0], v[n - 1]);
}

static void close_tag(const char* tag){
    if(InterlockedExchange(&g_open, 0) == 0) return;
    if(g_frames || g_calls){
        printf("PCDSP spans frames=%ld calls=%ld calls_per_frame=%.3f guard_flushes=%ld full_flushes=%ld errors=%ld spans=%ld pixels=%ld rpc_ms_per_frame=%.4f dsp_us_per_frame=%.2f\n",
               g_frames, g_calls, g_frames ? (double)g_calls / g_frames : 0.0, g_guard_flushes, g_full_flushes, g_errors, g_spans_total, g_pix_total,
               g_frames ? g_rpc_ms_total / g_frames : 0.0, g_frames ? g_dsp_us_total / g_frames : 0.0);
        printf("PCDSP vtcm on=%d staged_bytes_per_frame=%.0f staged_surfs_per_frame=%.1f last_code=%d\n", g_vtcm, g_frames ? (double)g_staged_bytes / g_frames : 0.0, g_frames ? (double)g_staged_surfs / g_frames : 0.0, g_vtcm_code);
        printf("PCDSP inplace_rebuilds_of_queued same_bytes=%d changed=%d\n", pc_inplace_same, pc_inplace_diff);
        printf("PCDSP hvx on=%d threads=%d tex_hvx=%ld tex_fallback=%ld z_hvx=%ld z_scalar=%ld gs_hvxrc=%d v2_calls=%ld stage_us_per_v2_call=%.1f mirror=%d dirty_bytes_per_frame=%.0f full_copies=%ld\n", g_hvx, g_threads, g_thvx, g_tfb, g_zhvx, g_zsc, g_gsok, g_stage_calls, g_stage_calls ? g_stage_us / g_stage_calls : 0.0,
               g_mirror, g_frames ? (double)g_dirty_bytes / g_frames : 0.0, g_full_copies);
        printf("PCDSP turb on=%d sick=%d spans=%ld pixels=%ld pixels_per_frame=%.0f texture_records=%ld hvx=%d dsp_spans_hvx=%ld dsp_spans_scalar=%ld | dsp_us phase2_per_job=%.1f water_longest_thread_per_job_with_water=%.1f jobs_with_water=%ld water_ns_per_px(longest thread / all px)=%.2f\n", g_turb, g_turb_sick, g_turb_spans, g_turb_pix,
               g_frames ? (double)g_turb_pix / g_frames : 0.0, g_turb_texrecs, g_turb_hvx, g_turb_hvx_spans, g_turb_sc_spans,
               g_ph_jobs ? g_ph2_us / g_ph_jobs : 0.0, g_ph3_jobs ? g_ph3_us / g_ph3_jobs : 0.0, g_ph3_jobs, g_turb_pix ? 1000.0 * g_ph3_us / (double)g_turb_pix : 0.0);
        if(g_pipe) printf("PCDSP pipe async_partial_jobs=%ld stream_buffers=%d\n", g_async_partial, g_nsb);
        if(g_pipe) printf("PCDSP pipe async_jobs=%ld sync_jobs=%ld waits=%ld wait_ms_per_frame=%.4f async_full_copy_errors=%ld sync_because_full_copy=%ld inline_dirty_bytes_per_frame=%.0f\n",
                          g_async_jobs, g_sync_jobs, g_waits, g_frames ? g_wait_ms / g_frames : 0.0, g_async_full, g_sync_full, g_frames ? (double)g_inline_bytes / g_frames : 0.0);
        if(g_mcheck_calls) printf("PCDSP mirrorcheck calls=%ld differing_bytes_total=%ld first_bad_offset=%d\n", g_mcheck_calls, g_mcheck_bad, g_mcheck_first);
        stat_line("frame_rpc_ms", g_frame_ms, g_nf);
        stat_line("frame_dsp_us", g_frame_us, g_nf);
        if(g_pipe){ stat_line("frame_wait_ms", g_frame_wait, g_nfw); stat_line("job_start_latency_ms", g_lat, g_nlat); stat_line("job_cpu_overlap_ms", g_dur, g_nlat); }
        fflush(stdout);
    }
    int st[10] = { -111, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, rr, rs = -1, rh = -1;
    shut_phase("release");
    rr = pxquake_release(H, st, 10);
    if(st[8] > 0){ g_shut_timeouts++; printf("PCDSP SHUTDOWN TIMEOUT: %d HVX worker(s) did not reach their end within 0.5 s (not joined; the session close tears the PD down)\n", st[8]); }
    printf("PCDSP vtcm release_requests_from_other_clients=%d\n", st[9]);
    {   const int wbad = (rr != 0 || st[0] != 364 || st[5] != 0 || st[3] != st[4]);
        printf("PCDSP workers join: joined=%d clean(QURT_EOK+0x574B)=%d not_clean=%d -> %s\n", st[3], st[4], st[5],
               wbad ? "JOIN WITNESS FAILED (HVX workers)" : st[3] ? "JOINED" : "NO WORKER WAS RUNNING");
        if(wbad){ g_join_bad = 1; printf("PCDSP JOIN WITNESS FAILED (workers: last join rc=%d exit=0x%X)\n", st[6], (unsigned)st[7]); }
    }
    shut_phase("free_buffers");
    free_all();
    shut_phase("session_close");
    if(p_sess){ struct remote_rpc_session_close sc; sc.domain = CDSP_DOMAIN_ID; rpc_check("session_close", 0); rs = p_sess((uint32_t)FASTRPC_SESSION_CLOSE, &sc, sizeof sc); }
    shut_phase("handle_close");
    if(p_close) rh = p_close(H);
    printf("PCDSP recycles=%ld failed=%ld ms_mean=%.2f ms_max=%.2f (every %.1f s; 0 = off)\n", g_recycles, g_recycle_bad,
           g_recycles ? g_recycle_ms / g_recycles : 0.0, g_recycle_max_ms, g_recycle_ms_set / 1000.0);
    printf("PCDSP rpc owner_thread=%lu in_window_message=%ld from_other_threads=%ld%s\n", (unsigned long)g_owner_tid, g_rpc_in_msg, g_rpc_foreign,
           (g_rpc_in_msg || g_rpc_foreign) ? "  <== WITNESS FAILED" : "");
    H = 0;
    printf("PCDSP close/%s release rc=%d status=%d calls_seen_by_dsp=%d session_close rc=%d handle_close rc=%d%s\n", tag, rr, st[0], st[2], rs, rh,
           (rs == 0 && rh == 44) ? " (44 = already closed by the session: the close was real)" : (rs == 0 && rh == 0) ? " (ok)" : " <== check");
    fflush(stdout);
}
static const char* g_ctag;
static void w_close(void* a){ (void)a; close_tag(g_ctag); }
static void dq_teardown(void);
double pcdsp_wait(void);
static void close_any_inner(const char* tag);
static void close_any(const char* tag){
    HANDLE wt = NULL;
    if(g_open && !g_closing){
        g_closing = 1; g_shut_t0 = now_ms(); g_shut_done = 0;
        wt = CreateThread(NULL, 0, shut_watch, NULL, 0, NULL);
        shut_phase(tag);
    }
    close_any_inner(tag);
    if(wt){ InterlockedExchange(&g_shut_done, 1); shut_phase("done"); WaitForSingleObject(wt, 200); CloseHandle(wt); }
    const int code = pcdsp_exit_code();
    if(code){ printf("PCDSP run FAILED: exit code %d (%s)\n", code, code == 9 ? "a DSP RPC inside a window-message handler" : code == 7 ? "a DSP thread did not end cleanly" : code == 10 ? "the DSP did not apply -dsplpm/-dsplat (LPM witness)" : code == 11 ? "the DSP was lost during the session (the CPU rendered on)" : "a bounded shutdown wait expired");
              fflush(stdout); fflush(stderr); ExitProcess(code); }
}
static void close_any_inner(const char* tag){
    if(g_dq && g_open && g_dsp_lost){                  /* phase 9: a lost session */
        shut_phase("lost_stop_flag");
        const int acked = lost_stop_acked();
        dsp_log_line("lost-shutdown");
        if(acked){
            printf("PCDSP lost: the DSP still acknowledged its stop - the normal bounded close follows\n"); fflush(stdout);
            g_dsp_lost = 0;                            /* RPCs are safe again: the DSP answers */
            dq_teardown(); close_tag(tag); return;
        }
        printf("PCDSP lost: no stop ack in 300 ms - NO RPC into a session that does not answer; the driver reclaims it at exit (exit code 11)\n");
        fflush(stdout);
        InterlockedExchange(&g_open, 0);
        return;
    }
    if(g_dq){ if(g_open){ dsp_log_line("shutdown"); shut_phase("wait_jobs"); g_wait_lim_ms = 1500.0; pcdsp_wait(); dq_teardown(); } close_tag(tag); return; }
    if(!g_pipe || !g_wthr){ close_tag(tag); return; }
    shut_phase("wait_worker");
    if(g_wbusy && WaitForSingleObject(g_wdone, 3000) == WAIT_TIMEOUT){   /* phase 7: bounded report; the RPC in flight belongs to the worker */
        g_shut_timeouts++;
        printf("PCDSP SHUTDOWN TIMEOUT: the worker's RPC did not return in 3 s - waiting on (closing under a running RPC is not safe; never killed)\n"); fflush(stdout);
        WaitForSingleObject(g_wdone, INFINITE);
    }
    g_wbusy = 0;
    g_ctag = tag; w_run(w_close, NULL);
    g_wquit = 1; SetEvent(g_wgo); WaitForSingleObject(g_wthr, 5000); CloseHandle(g_wthr); g_wthr = NULL;
}
void pcdsp_close(void){ close_any("explicit"); }

/* ---------------------------------------------------------------- span queue */
static unsigned char* g_fb; static int g_fblen; static short* g_zb; static int g_zbn; static unsigned char* g_cache; static int g_cachelen;
static int* g_cmd; static int g_ncmd; static int g_cmdcap;
#define NSB 4
static int* g_cmdbuf[NSB]; static int g_cmdcur; static int g_nsb = 1;
static int g_mcheck = 0;
#define MAXBLK 8192
static const unsigned char* g_blo[MAXBLK]; static const unsigned char* g_bhi[MAXBLK]; static int g_nblk;
static int g_sick_from = -1, g_first_span_of_frame = 1;
static int g_vtcm = 0;       /* 1 = SURF records ask the DSP to stage the block into VTCM (op 4) */
static long g_staged_bytes, g_staged_surfs; static int g_vtcm_code = -99;
static int g_hvx = 0;        /* 1 = HVX span kernels on the DSP (-dspspans 2) */
static long g_thvx, g_tfb, g_zhvx, g_zsc; static int g_gsok = -1;
static int g_diag = 0;       /* DIAGNOSTIC timing only (wrong pictures): bit 0 no texture loop, bit 1 no z */
static int g_threads = 0;    /* 0 = the serial DSP path; >= 1 = the threaded path with that many DSP threads */
/* v2 tables (HVX mode): surfaces (cache_off, nbytes, voff, cw) and spans (pos, surface#) */
#define V2_MAXS 8192
#define V2_MAXI 65536
#define V2_VBUDGET (6u << 20)
static int g_st[4 * V2_MAXS], g_nst; static int g_si[2 * V2_MAXI], g_nsi; static unsigned g_vused; static int g_cursurf = -1, g_hdr = 2;
static const unsigned char *g_cur_tex, *g_cur_blo, *g_cur_bhi; static int g_cur_cw;
/* the VTCM mirror: the surface cache at the same offsets in VTCM; the host lists what the CPU rebuilt since the last call */
#define V2_MAXD 65536
static int g_mirror = 1, g_dirty[2 * V2_MAXD], g_ndirty, g_dirty_all; static long g_dirty_bytes, g_full_copies;
static long g_sipix;
static void q_reset(void){ g_ncmd = 0; g_nblk = 0; g_nst = 0; g_nsi = 0; g_sipix = 0; g_vused = 0; g_cursurf = -1; g_ntu = 0; g_turb_texpos = -1;
    if(g_cmd){ g_cmd[g_ncmd++] = 3; g_cmd[g_ncmd++] = g_threads | (g_threads ? (g_diag << 8) | (g_hvx << 12) | (g_hvx << 13) | ((g_hvx & g_mirror) << 15)
                                                                       | ((g_hvx & g_mirror & g_pipe) << 16) | ((g_hvx & g_mirror & g_mcheck) << 17)
                                                                       | ((g_hvx & g_mirror & g_turb) << 18) | ((g_hvx & g_mirror & g_turb & g_turb_hvx) << 19) : 0);
              if(g_hvx && g_threads){ g_cmd[g_ncmd++] = 7; g_cmd[g_ncmd++] = 0; g_cmd[g_ncmd++] = 0; g_cmd[g_ncmd++] = 0;
                                      if(g_mirror){ g_cmd[g_ncmd++] = 0; g_cmd[g_ncmd++] = 0; } } }
    g_hdr = g_ncmd; }
static int v2_on(void){ return g_hvx && g_threads && g_hdr == (g_mirror ? 8 : 6); }
static int g_sic[V2_MAXI]; static long g_sipix;
void pcdsp_set_mirror(int on){ g_mirror = on ? 1 : 0; q_reset(); }
void pcdsp_set_mirrorcheck(int on){ g_mcheck = on ? 1 : 0; q_reset(); }
void pcdsp_dirty_all(void){ g_dirty_all = 1; }
void pcdsp_set_target(unsigned char* fb, short* zb){ g_fb = fb; g_zb = zb; }
static int g_test_nohdr = 0;   /* known-sick control of the mirror check: header reports dropped */
void pcdsp_test_nohdr(int on){ g_test_nohdr = on; }
void pcdsp_dirty(const unsigned char* lo, const unsigned char* hi){
    if(!g_cache || lo < g_cache || hi > g_cache + g_cachelen || hi <= lo) return;
    if(g_test_nohdr && hi - lo <= 64) return;
    if(g_ndirty >= V2_MAXD){ g_dirty_all = 1; return; }
    g_dirty[2 * g_ndirty] = (int)(lo - g_cache); g_dirty[2 * g_ndirty + 1] = (int)(hi - lo); g_ndirty++;
}
void pcdsp_set_hvx(int on){ g_hvx = on ? 1 : 0; if(g_hvx){ g_vtcm = 1; if(!g_threads) g_threads = 4; } q_reset(); }
void pcdsp_set_diag(int d){ g_diag = d & 127; q_reset(); }
void pcdsp_set_vtcm(int on){ g_vtcm = on ? 1 : 0; }
void pcdsp_set_threads(int n){ g_threads = n < 0 ? 0 : n > 6 ? 6 : n; q_reset(); }

void pcdsp_set_sick(int from_frame){ g_sick_from = from_frame; }
void pcdsp_set_buffers(unsigned char* fb, int fblen, short* zb, int zbn, unsigned char* cache, int cachelen){
    g_fb = fb; g_fblen = fblen; g_zb = zb; g_zbn = zbn; g_cache = cache; g_cachelen = cachelen;
    if(!g_cmd){
        g_cmdcap = 4 * 1024 * 1024;                            /* 16 MB of int32 (640x480 needs ~0.75 MB) */
        g_nsb = g_dq ? NSB : g_pipe ? 2 : 1;           /* phase 5: the mailbox mode keeps up to 4 jobs in flight */
        if(g_dq) g_cmdcap = 2 * 1024 * 1024;           /* 8 MB per stream (1280x960 needs ~3 MB) */
        int okb = 1;
        for(int k = 0; k < g_nsb; k++){ g_cmdbuf[k] = (int*)pcdsp_alloc(g_cmdcap * 4); if(!g_cmdbuf[k]) okb = 0; }
        g_cmdcur = 0; g_cmd = g_cmdbuf[0];
        if(!okb){ g_cmd = NULL; g_cmdcap = 0; }
    }
    q_reset();
}
typedef struct { int* cmd; int ncmd; unsigned char* fb; short* zb; int st[26]; int warm; int rc; double rpc_ms; int async; double t_sub, t_start, t_end; unsigned seq; unsigned hseq; } pq_job_h;
static pq_job_h g_jobs[NSB]; static int g_fifo[NSB], g_fh = 0, g_fn = 0;   /* jobs in flight, oldest first */
static int g_async_ok = 0; static long g_async_partial = 0;
void pcdsp_set_async_ok(int on){ g_async_ok = on; }
static int g_primed = 0;                                         /* the DSP mirror has had its full copy */
static void job_call(void* a){ pq_job_h* j = (pq_job_h*)a; double t0 = now_ms(); j->t_start = t0;
    j->rc = pxquake_spans(H, j->cmd, j->ncmd, g_cache, g_cachelen, j->fb, g_fblen, j->zb, g_zbn, j->st, 26); j->t_end = now_ms(); j->rpc_ms = j->t_end - t0; }
static void job_fold(pq_job_h* j){
    if(g_dsp_lost && (j->rc == -4 || j->rc == -3)) return;   /* phase 9: abandoned with the lost DSP, not an error per frame */
    int* st = j->st; int rc = j->rc; double dt = j->rpc_ms;
    if(st[18] >= 0){ g_stage_us += st[18]; g_stage_calls++; }
    g_thvx += st[13]; g_tfb += st[14]; g_zhvx += st[15]; g_zsc += st[16]; g_gsok = st[17];
    g_staged_bytes += st[10]; g_staged_surfs += st[11]; g_vtcm_code = st[12];
    if(j->async && st[19] == -1){ g_async_full++; g_errors++; dlog("ERROR: an asynchronous job made a FULL mirror copy from the shared cache (race)"); }
    if(g_mcheck && !j->async){ g_mcheck_calls++; g_mcheck_bad += st[20]; if(st[20] && g_mcheck_first < 0) g_mcheck_first = st[21]; }
    if(st[22] >= 0) g_turb_hvx_spans += st[22]; if(st[23] >= 0) g_turb_sc_spans += st[23];   /* phase 6 witness */
    if(st[24] >= 0){ g_ph2_us += st[24]; g_ph_jobs++; } if(st[25] > 0){ g_ph3_us += st[25]; g_ph3_jobs++; }
    g_calls++; g_frame_calls++; g_rpc_ms_total += dt; g_frame_rpc_ms += dt; g_dsp_us_total += st[4]; g_frame_dsp_us += st[4];
    g_spans_total += st[1]; g_pix_total += st[2];
    if(rc != 0 || st[0] != 0x5A || st[5] != 0 || st[6] != -1 || st[8] != 0){
        g_errors++;
        if(g_errors < 10) dlog("span call FAILED rc=%d st=%d spans=%d pix=%d oob_texel=%d badop=%d oob_dest=%d", rc, st[0], st[1], st[2], st[5], st[6], st[8]);
    }
}
/* wait for the job in flight (if any) and fold its statistics in; returns the ms the main thread waited */
/* ---- phase 5: the mailbox (see skel/pxquake/pq_mbox.h for the DSP side and the layout) */
#include "pq_mbox_layout.h"
static int g_mb_was_started = 0, g_join_test = 0;
static int g_lpm = 0; void pcdsp_set_lpm(int n){ g_lpm = n < 0 ? 0 : n > 3 ? 3 : n; }
static int g_lat2_us = 0; void pcdsp_set_lat(int us){ g_lat2_us = us < 0 ? 0 : us; }
static int g_clock = 0; void pcdsp_set_clock(int m){ g_clock = m < 0 || m > 2 ? 0 : m; }   /* phase 9: -dspclock */
static const char* const k_clock[3] = { "turbo", "nom", "dcvs" };
static double g_sess_t0 = 0.0;
static int g_spinidle = 1; void pcdsp_set_spinidle(int on){ g_spinidle = on ? 1 : 0; }   /* phase 7: DEFAULT ON (a sleeping DSP thread froze after ~0.8 s idle) */
static int g_lpm_checked = 0, g_lpm_bad = 0;
int pcdsp_lpm_ok(void){ return !g_lpm_bad; }
static void lpm_check(void);
static double g_last_submit_ms = 0.0; static unsigned g_seen_epoch = 0; static long g_gap_syncs = 0, g_epoch_syncs = 0;
static int g_warm_ms = 0; static long g_warm_jobs = 0; static int* g_warm_cmd = NULL;
void pcdsp_set_keepwarm(int ms){ g_warm_ms = ms > 0 ? ms : 0; }
void pcdsp_set_join_test(int f){ g_join_test = f; }
static pq_mb* g_mb = NULL; static int g_mb_started = 0; static unsigned g_mb_spin = 500, g_mb_poll = 200;   /* measured: qurt_sleep below ~100 us does not sleep; 200 us does */
static double g_dq_submit_ms, g_dq_wait_ms; static long g_dq_frames; static double g_mb_inv_us, g_mb_job_us, g_mb_flush_us;
/* phase 7: after the first job: did the DSP apply what was asked? (a mismatch fails the run, exit 10) */
static void lpm_check(void){
    if(g_lpm_checked || !g_mb) return;
    g_lpm_checked = 1;
    const unsigned vs = g_mb->d_v_sleep, applied = vs & 255u, set = vs >> 8;
    const int lpm_ok = (unsigned)g_lpm == applied && (!g_lpm || set) && g_mb->d_v_n >= 1 && (!g_lpm || g_mb->d_v_dcvs_rc == 0) && g_mb->d_lpm_req == (unsigned)g_lpm;
    const int lat_ok = (unsigned)g_lat2_us == g_mb->d_v_lat2 && (!g_lat2_us || g_mb->d_v_lat2_rc == 0);
    const unsigned vc = g_mb->d_v_clock;           /* phase 9: the clock vote the DSP applied */
    const int clk_ok = (vc & 0xFFu) == (unsigned)g_clock && !(vc & 0x8000u) && g_mb->d_v_dcvs_rc == 0 && (g_clock != 2 || ((vc >> 8) & 1u));
    printf("PCDSP clock requested=%s applied=%s dcvs_enabled=%u read_ok=%u dcvs_rc=%d -> %s\n", k_clock[g_clock],
           (vc & 0xFFu) < 3u ? k_clock[vc & 0xFFu] : "?", (vc >> 8) & 1u, (vc & 0x8000u) ? 0u : 1u, (int)g_mb->d_v_dcvs_rc,
           clk_ok ? "APPLIED" : "CLOCK WITNESS FAILED (exit code 10)");
    if(!clk_ok) g_lpm_bad = 1;
    printf("PCDSP lpm requested=%d applied=%u (set=%u) dsp_read=%u dcvs_rc=%d votes=%u dcvs_latency_us=%u | lat2 requested=%d applied=%u rc=%d -> %s\n",
           g_lpm, applied, set, g_mb->d_lpm_req, (int)g_mb->d_v_dcvs_rc, g_mb->d_v_n, g_mb->d_v_lat, g_lat2_us, g_mb->d_v_lat2, (int)g_mb->d_v_lat2_rc,
           (lpm_ok && lat_ok) ? "APPLIED" : "LPM WITNESS FAILED (exit code 10)");
    fflush(stdout);
    if(!(lpm_ok && lat_ok)) g_lpm_bad = 1;
}
void pcdsp_set_mb_timing(int spin_us, int poll_us){ if(spin_us >= 0) g_mb_spin = (unsigned)spin_us; if(poll_us > 0) g_mb_poll = (unsigned)poll_us; }
static int mb_ensure(void){
    if(g_mb_started) return 0;
    int fds[NB], sz[NB], n = 0;
    const int mi = buf_index(g_mb);
    if(mi < 0 || !g_b[mi].mapped){ seterr("mailbox not mapped"); return 30; }
    fds[n] = g_b[mi].fd; sz[n] = g_b[mi].n; n++;
    for(int i = 0; i < NB; i++) if(g_b[i].p && g_b[i].mapped && i != mi){ fds[n] = g_b[i].fd; sz[n] = g_b[i].n; n++; }
    g_mb->poll_us = g_mb_poll; g_mb->spin_us = g_mb_spin; g_mb->stop = 0; g_mb->lpm = (uint32_t)g_lpm; g_mb->lat = (uint32_t)g_lat2_us; g_mb->spinidle = (uint32_t)g_spinidle; g_mb->clock = (uint32_t)g_clock; if(g_sess_t0 == 0.0) g_sess_t0 = now_ms();
    g_mb->recycle = 0;
    g_mb->test_flags = (uint32_t)g_join_test;          /* phase 9: before the start (bit 6 acts during the run) */
    int st[4] = { -111, 0, 0, 0 };
    int rc = pxquake_mb_start(H, fds, n, sz, n, st, 4);
    if(rc || st[0] != 0x73){ seterr("mb_start failed rc=%d st=%d/%d/%d", rc, st[0], st[1], st[2]); return 31; }
    g_mb_started = 1; g_mb_t0 = now_ms();
    dlog("DSP mailbox worker started: %d buffers mapped on the DSP, thread priority %d, spin %u us, poll %u us", st[1], st[2], g_mb_spin, g_mb_poll);
    return 0;
}
static void dq_submit(pq_job_h* j){
    j->rc = 0;
    if(g_dsp_lost){ j->rc = -4; return; }              /* phase 9: nothing goes to a lost DSP */
    if(mb_ensure()){ j->rc = -1; return; }
    const int ic = buf_index(j->cmd), ifb = buf_index(j->fb), iz = buf_index(j->zb), ica = buf_index(g_cache);
    if(ic < 0 || ifb < 0 || iz < 0 || ica < 0){ j->rc = -2; dlog("mailbox: a buffer is not mapped"); return; }
    double t0 = now_ms();
    j->t_start = t0;
    j->seq = ++g_dqseq;
    pq_slot* sl = &g_mb->slot[j->seq % MB_SLOTS];       /* free: at most MB_SLOTS jobs are ever unfinished */
    sl->fd_cmds = g_b[ic].fd; sl->off_cmds = (int)((char*)j->cmd - (char*)g_b[ic].p);
    sl->fd_fb = g_b[ifb].fd;  sl->off_fb = (int)((char*)j->fb - (char*)g_b[ifb].p);
    sl->fd_zb = g_b[iz].fd;   sl->off_zb = (int)((char*)j->zb - (char*)g_b[iz].p);
    sl->fd_cache = g_b[ica].fd;
    sl->cmdsLen = j->ncmd; sl->cacheLen = g_cachelen; sl->fbLen = g_fblen; sl->zbLen = g_zbn;
    sl->flags = (j->async ? 0 : 1) | (g_dq_noflush ? 4 : 0);   /* bit 0: invalidate the shared cache; bit 2: TEST, no cache maintenance */
    MemoryBarrier();
    g_mb->req_seq = j->seq;
    g_dq_submit_ms += now_ms() - t0;
    g_last_submit_ms = now_ms();
}
static void dq_wait(pq_job_h* j){
    if(j->rc){ j->st[0] = -111; return; }
    if(g_dsp_lost){ j->rc = -4; j->st[0] = -111; return; }   /* phase 9: never wait on a lost DSP */
    double t0 = now_ms(), lim = t0 + g_wait_lim_ms;     /* phase 7: 5 s while playing, 1.5 s at the shutdown */
    unsigned spins = 0;
    while((int32_t)(g_mb->rsp_seq - j->seq) < 0){
        YieldProcessor();
        if((++spins & 1023) == 0 && now_ms() > lim){ dlog("mailbox: no answer from the DSP for %.0f ms (seq %u, last %u)", g_wait_lim_ms, j->seq, g_mb->rsp_seq); j->rc = -3; j->st[0] = -111;
            if(g_closing){ g_shut_timeouts++; printf("PCDSP SHUTDOWN TIMEOUT: job %u not answered in %.0f ms\n", j->seq, g_wait_lim_ms); fflush(stdout); }
            else dsp_mark_lost("a job was not answered");   /* phase 9: from here on the CPU renders */
            return; }
    }
    MemoryBarrier();
    double t1 = now_ms();
    if(!j->warm){ g_dq_wait_ms += t1 - t0; g_dq_frames++; }
    const pq_slot* sl = &g_mb->slot[j->seq % MB_SLOTS];
    for(int i = 0; i < 26; i++) j->st[i] = sl->st[i];
    g_mb_inv_us += sl->dsp_us_inv; g_mb_job_us += sl->dsp_us_job; g_mb_flush_us += sl->dsp_us_flush;
    {   static uint32_t last = 0;                     /* phase 9: the resource line, every 1000 DSP jobs */
        if(g_mb->r_jobs != last && g_mb->r_jobs){ last = g_mb->r_jobs;
            PROCESS_MEMORY_COUNTERS_EX pm; memset(&pm, 0, sizeof pm); pm.cb = sizeof pm;
            K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
            DWORD hc = 0; GetProcessHandleCount(GetCurrentProcess(), &hc);
            printf("PCDSP res jobs=%u heap_free_kb=%u heap_used_kb=%u segs=%u/%u memrc=%d stack_mb=%u stack_wk=%u vtcm_acq=%u votes=%u forks=%u cacheops_k=%u maxjob_us=%u nmb=%u calls=%u dsp_ms=%u clock=%s clk_mhz=%u sess_s=%.1f | host commit_mb=%.1f handles=%lu\n",
                   g_mb->r_jobs, g_mb->r_heap_free_kb, g_mb->r_heap_used_kb, g_mb->r_seg_free, g_mb->r_seg_used, (int)g_mb->r_mem_rc, g_mb->r_stack_mb, g_mb->r_stack_wk,
                   g_mb->r_vtcm_acq, g_mb->r_votes, g_mb->r_forks, g_mb->r_cacheops_k, g_mb->r_maxjob_us, g_mb->r_nmb, g_mb->r_calls, g_mb->r_dsp_ms, k_clock[g_clock], g_mb->d_clk_mhz, (now_ms() - g_sess_t0) / 1000.0,
                   pm.PrivateUsage / 1048576.0, (unsigned long)hc);
            fflush(stdout); } }
    if(!g_lpm_checked && sl->st[0] == 0x5A) lpm_check();    /* phase 7: the first answered job has voted */
    j->t_end = t1; j->rpc_ms = t1 - j->t_start; j->rc = 0;
}
static int dq_setup(void){
    HMODULE lib = g_cdsprpc;
    p_fmm = (fn_fmm)GetProcAddress(lib, "fastrpc_mmap"); p_fmu = (fn_fmu)GetProcAddress(lib, "fastrpc_munmap");
    if(!p_fmm || !p_fmu){ seterr("libcdsprpc lacks fastrpc_mmap/fastrpc_munmap"); return 20; }
    g_mb = (pq_mb*)pcdsp_alloc(4096);
    g_warm_cmd = (int*)pcdsp_alloc(4096);              /* phase 7: the empty job of -dspqwarm */
    if(!g_mb){ seterr("no mailbox page"); return 21; }
    memset((void*)g_mb, 0, sizeof *g_mb);
    if(!g_b[buf_index(g_mb)].mapped){ seterr("mailbox page could not be mapped for the DSP"); return 22; }
    return 0;
}
static void dq_teardown(void){
    if(!g_mb) return;
    int st[12] = { -111, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, qs = -1;
    if(g_mb_started){
        g_mb_was_started = 1;
        g_mb->test_flags = (uint32_t)g_join_test;
        MemoryBarrier();
        shut_phase("stop_flag");                          /* phase 7: the stop without an RPC first */
        g_mb->stop = 1; MemoryBarrier();
        {   const double a0 = now_ms(); int said = 0;
            while(g_mb->d_stop_ack != 0x53544F50u){
                const double w = now_ms() - a0;
                if(w > 3000.0 && !said){ said = 1; g_shut_timeouts++;
                    printf("PCDSP SHUTDOWN: NO STOP ACK from the DSP mailbox thread after 3 s - no RPC yet, waiting up to 30 s\n"); fflush(stdout); dsp_log_line("no-ack"); }
                if(w > 30000.0){
                    dsp_log_line("no-ack-30s");
                    printf("PCDSP SHUTDOWN: no stop ack in 30 s - NOT issuing RPCs into a PD that does not answer its mailbox; the session stays open: do NOT kill the process, restart the computer\n"); fflush(stdout);
                    shut_phase("no_ack_waiting");
                    for(;;) Sleep(1000);                   /* the watchdog keeps reporting; nothing is killed */
                }
                Sleep(1);
            }
            printf("PCDSP stop ack after %.1f ms\n", now_ms() - a0);
            dsp_log_line("acked");
        }
        shut_phase("mb_stop");
        qs = pxquake_mb_stop(H, st, 12);
        printf("PCDSP vote at stop: votes=%d sleep_disable=%d (set=%d) dcvs_rc=%d lpm_dsp_read=%d lat2=%d rc=%d\n", st[6], st[7] & 255, st[7] >> 8, st[8], st[9], st[10], st[11]);
        if(qs == 0 && st[1] == -101){                     /* phase 7: the DSP-side 1 s bound expired */
            g_shut_timeouts++;
            printf("PCDSP SHUTDOWN TIMEOUT: the DSP mailbox thread did not reach its end within 1 s - retrying the stop once\n"); fflush(stdout);
            shut_phase("mb_stop_retry");
            for(int i = 0; i < 12; i++) st[i] = 0; st[0] = -111;
            qs = pxquake_mb_stop(H, st, 12);
            if(qs == 0 && st[1] == -101) printf("PCDSP SHUTDOWN TIMEOUT: still running after the retry - not joined; the session close tears the PD down\n");
        }
        g_mb_started = 0;
    }
    if(!g_mb_was_started) printf("PCDSP mailbox join: the mailbox thread was never started%s\n", g_dq_frames ? "  <== frames were sent" : "");
    else {
        const int jr = st[1], ex = st[5];
        const int ok = (qs == 0 && st[0] == 0x74 && jr == 0 && ex == 0x4A4F);
        printf("PCDSP mailbox join: qurt_thread_join=%d exit_code=0x%X -> %s\n", jr, (unsigned)ex,
               ok ? "JOINED (QURT_EOK + 0x4A4F)" : (qs != 0 || st[0] != 0x74) ? "STOP CALL FAILED" : jr == -100 ? "NO THREAD WAS RUNNING" : jr == 30 ? "THREAD HAD EXITED BEFORE THE JOIN (QURT_ENOTHREAD, exit code lost)" : jr != 0 ? "JOIN FAILED" : "JOINED BUT NO CLEAN EXIT CODE");
        if(!ok) g_join_bad = 1;
    }
    if(!g_mb_was_started && g_dq_frames) g_join_bad = 1;
    if(g_join_bad) printf("PCDSP JOIN WITNESS FAILED (mailbox thread)\n");
    printf("PCDSP mailbox stop: mb_stop rc=%d status=0x%x join=%d jobs_on_dsp=%d idle_sleeps=%d polls=%d | frames=%ld submit_ms_per_frame=%.4f wait_ms_per_frame=%.4f | dsp_us_per_frame invalidate=%.1f job=%.1f flush=%.1f\n",
           qs, (unsigned)st[0], st[1], st[2], st[3], st[4], g_dq_frames, g_dq_frames ? g_dq_submit_ms / g_dq_frames : 0.0, g_dq_frames ? g_dq_wait_ms / g_dq_frames : 0.0,
           g_dq_frames ? g_mb_inv_us / g_dq_frames : 0.0, g_dq_frames ? g_mb_job_us / g_dq_frames : 0.0, g_dq_frames ? g_mb_flush_us / g_dq_frames : 0.0);
    fflush(stdout);
}
/* -dspbench N W H -dspqbench: the mailbox's round trip (empty request) and an empty frame handoff with all buffers */
int pcdsp_dqbench(int n, int w, int h){
    int bad = 0; if(n < 10) n = 10;
    pcdsp_set_dq(1);
    if(pcdsp_open() != 0){ printf("PCDSP dqbench error=open %s\n", g_err); return 2; }
    const int fbn = w * h, cachen = (int)(1312.0 * 1024.0 * ((double)fbn / (640.0 * 480.0)));
    unsigned char* fb = (unsigned char*)pcdsp_alloc(fbn); short* zb = (short*)pcdsp_alloc(fbn * 2);
    unsigned char* cache = (unsigned char*)pcdsp_alloc(cachen); int* cmd = (int*)pcdsp_alloc(4 << 20);
    float* t = (float*)malloc(sizeof(float) * (size_t)n);
    if(!fb || !zb || !cache || !cmd || !t){ printf("PCDSP dqbench error=alloc\n"); pcdsp_close(); return 2; }
    g_fb = fb; g_fblen = fbn; g_zb = zb; g_zbn = fbn; g_cache = cache; g_cachelen = cachen;
    printf("PCDSP dqbench w=%d h=%d n=%d spin_us=%u poll_us=%u\n", w, h, n, g_mb_spin, g_mb_poll);
    static const int gaps_us[4] = { 0, 200, 700, 2000 };       /* the host's work between two handoffs */
    for(int gi = 0; gi < 4; gi++){
        for(int i = 0; i < n + 20; i++){
            pq_job_h j; memset(&j, 0, sizeof j); cmd[0] = 0; j.cmd = cmd; j.ncmd = 1; j.fb = fb; j.zb = zb; j.async = 1;
            if(gaps_us[gi]){ double g0 = now_ms(); while(now_ms() - g0 < gaps_us[gi] / 1000.0) YieldProcessor(); }
            double a = now_ms(); dq_submit(&j); dq_wait(&j);
            if(i >= 20) t[i - 20] = (float)(now_ms() - a);
            if(j.rc || j.st[0] != 0x5A) bad++;
        }
        char nm[64]; snprintf(nm, sizeof nm, "mb_frame_empty_ms(after %d us host work)", gaps_us[gi]); stat_line(nm, t, n);
    }
    printf("PCDSP dqbench bad=%d\n", bad);
    free(t);
    pcdsp_close();
    return bad ? 1 : 0;
}

static double wait_one(void){
    if(g_fn <= 0) return 0.0;
    if(g_dsp_lost){ g_fn = 0; return 0.0; }           /* phase 9: the jobs in flight are abandoned */
    pq_job_h* j = &g_jobs[g_fifo[g_fh]];
    double t0 = now_ms(); if(g_dq) dq_wait(j); else w_wait(); double w = now_ms() - t0;
    if(g_nlat < 20000){ g_lat[g_nlat] = (float)(j->t_start - j->t_sub); g_dur[g_nlat] = (float)(t0 - j->t_sub); g_nlat++; }
    job_fold(j); g_fh = (g_fh + 1) % NSB; g_fn--; g_waits++; g_wait_ms += w;
    return w;
}
static int buf_inflight(int k){ for(int i = 0; i < g_fn; i++) if(g_fifo[(g_fh + i) % NSB] == k) return 1; return 0; }
/* wait for EVERY job in flight; returns the ms waited */
double pcdsp_wait(void){ double w = 0.0; if(g_dsp_lost){ g_fn = 0; return 0.0; } while(g_fn > 0) w += wait_one(); return w; }
static unsigned g_hseq = 0;
unsigned pcdsp_last_seq(void){ return g_hseq; }
/* wait for the jobs up to host sequence number s (in order); later ones keep running */
double pcdsp_wait_upto(unsigned s){ double w = 0.0; while(g_fn > 0 && (int)(g_jobs[g_fifo[g_fh]].hseq - s) <= 0) w += wait_one(); return w; }
int pcdsp_inflight(void){ return g_fn > 0; }
/* the current stream as an asynchronous job, then the next free stream buffer for the CPU */
static void submit_async(void){
    pq_job_h* j = &g_jobs[g_cmdcur]; j->cmd = g_cmd; j->ncmd = g_ncmd; j->fb = g_fb; j->zb = g_zb; j->async = 1;
    for(int i = 0; i < 26; i++) j->st[i] = 0; j->st[0] = -111; j->st[12] = -98; j->st[18] = -1; j->st[22] = j->st[23] = j->st[24] = j->st[25] = -1;
    j->hseq = ++g_hseq;
    g_fifo[(g_fh + g_fn) % NSB] = g_cmdcur; g_fn++; g_async_jobs++;
    j->t_sub = now_ms();
    if(g_dq) dq_submit(j); else w_submit(job_call, j);
    const int next = (g_cmdcur + 1) % g_nsb;
    while(buf_inflight(next)) wait_one();
    g_cmdcur = next; g_cmd = g_cmdbuf[g_cmdcur];
    q_reset();
}
static int needs_full(void){
    long need = 3L * g_ndirty; for(int i = 0; i < g_ndirty; i++) need += (g_dirty[2 * i + 1] + 3) / 4;
    return g_dirty_all || g_ncmd + 4L * g_nst + 2L * g_nsi + g_ntu + need + 4096 > g_cmdcap;
}
int pcdsp_queued(void){ return g_cmd && g_ncmd > g_hdr; }
/* phase 7: a job after a >= 40 ms gap, or after the DSP reported a VTCM release (epoch), runs synchronously: the DSP
 * releases the VTCM only after >= 50 ms without a job, so no asynchronous job can meet a lost mirror */
static int mb_resync(void){
    if(!g_dq || !g_mb || !g_mb_started) return 0;
    if(g_mb->d_vtcm_epoch != g_seen_epoch){ g_seen_epoch = g_mb->d_vtcm_epoch; g_epoch_syncs++; return 1; }
    if(g_last_submit_ms > 0.0 && now_ms() - g_last_submit_ms >= 40.0){ g_gap_syncs++; return 1; }
    return 0;
}
/* phase 9 experiment B: recycle the DSP side (see skel/pxquake/patch_fas9_recycle.py). Host thread, frame boundary. */
static int mb_ensure(void);
static void mb_recycle(void){
    const double t0 = now_ms(), age = (t0 - g_mb_t0) / 1000.0;
    pcdsp_wait();                                  /* every job in flight answered (or the DSP is lost: 2 s bound) */
    if(g_dsp_lost || !g_mb_started) return;
    g_mb->recycle = 3u; MemoryBarrier(); g_mb->stop = 1; MemoryBarrier();
    {   const double a0 = now_ms(); int ok = 0;
        while(now_ms() - a0 < 300.0){ if(g_mb->d_stop_ack == 0x53544F50u){ ok = 1; break; } YieldProcessor(); }
        if(!ok){ dsp_mark_lost("recycle: no stop ack in 300 ms"); return; } }   /* no RPC into a DSP that does not answer */
    int st[16]; for(int i = 0; i < 16; i++) st[i] = 0; st[0] = -111;
    const int qs = pxquake_mb_stop(H, st, 16);
    g_mb_started = 0; g_mb->recycle = 0;
    const int ok = qs == 0 && st[0] == 0x74 && st[1] == 0 && st[5] == 0x4A4F && st[15] == 0;
    const int dacq = st[12] - g_rc_acq, dwj = st[13] - g_rc_wj, dwc = st[14] - g_rc_wc;
    g_rc_acq = st[12]; g_rc_wj = st[13]; g_rc_wc = st[14];
    const int rs = ok ? mb_ensure() : -1;
    const double ms = now_ms() - t0;
    g_recycles++; g_recycle_ms += ms; if(ms > g_recycle_max_ms) g_recycle_max_ms = ms;
    printf("PCDSP recycle n=%ld thread_age_s=%.1f sess_s=%.1f ms=%.2f mb_stop rc=%d join=%d exit=0x%X workers_joined=+%d clean=+%d bad=%d vtcm_acq_total=%d (+%d since the last recycle) mb_start=%d -> %s\n",
           g_recycles, age, (t0 - g_sess_t0) / 1000.0, ms, qs, st[1], (unsigned)st[5], dwj, dwc, st[15], st[12], dacq, rs,
           ok && rs == 0 ? "OK" : "RECYCLE FAILED");
    fflush(stdout);
    if(!ok || rs){ g_recycle_bad++; if(!ok) g_join_bad = 1; dsp_mark_lost("recycle failed"); }   /* the CPU renders on */
}
/* -dspqwarm: an empty job while nothing is submitted (called once per host frame, from the message pump) */
void pcdsp_tick(void){
    if(g_test_lost_frame >= 0 && !g_dsp_lost && g_open && g_dq && g_frames >= g_test_lost_frame){
        pcdsp_wait();                                  /* TEST: the jobs in flight finish first, so every frame stays exact */
        dsp_mark_lost("TEST -pctest_dsplost");
    }
    if(g_dsp_lost) return;
    if(g_recycle_ms_set && g_dq && g_open && g_mb_started && !g_closing && !pcdsp_queued() && now_ms() - g_mb_t0 >= (double)g_recycle_ms_set)
        mb_recycle();                              /* phase 9 experiment B */
    if(g_dsp_lost) return;
    {   static double next = 0.0; const double t = now_ms();      /* phase 9: every 30 s, shared memory only */
        if(g_dq && g_open && g_mb_started && t >= next){ if(next > 0.0) dsp_log_line("periodic"); next = t + 30000.0; } }
    if(!g_warm_ms || !g_dq || !g_open || g_closing || !g_mb_started || g_fn != 0 || !g_warm_cmd) return;
    if(now_ms() - g_last_submit_ms < (double)g_warm_ms) return;
    pq_job_h j; memset(&j, 0, sizeof j); g_warm_cmd[0] = 0; j.cmd = g_warm_cmd; j.ncmd = 1; j.fb = g_fb; j.zb = g_zb; j.async = 1; j.warm = 1;
    dq_submit(&j); dq_wait(&j); g_warm_jobs++;
}
static int lost_stop_acked(void){
    if(!g_mb || !g_mb_started || g_test_lost_noack) return 0;
    g_mb->test_flags = (uint32_t)g_join_test; MemoryBarrier(); g_mb->stop = 1; MemoryBarrier();
    const double a0 = now_ms();
    while(now_ms() - a0 < 300.0){ if(g_mb->d_stop_ack == 0x53544F50u) return 1; Sleep(1); }
    return 0;
}
static void dsp_mark_lost(const char* why){
    if(g_dsp_lost) return;
    g_dsp_lost = 1; g_lost_exit = 1; g_wait_lim_ms = 150.0;
    printf("PCDSP lost -> CPU rendering (%s; frame %ld, last answered seq %u) - no further jobs, waits or RPCs to this session\n",
           why, g_frames, g_mb ? g_mb->rsp_seq : 0u);
    fflush(stdout);
    dsp_log_line("lost");
}
static void dsp_log_line(const char* why){
    if(!g_mb){ printf("PCDSP dsp_log(%s) no mailbox\n", why); fflush(stdout); return; }
    static const char* const sn[5] = { "-", "polling", "in_job", "idle_sleeping", "stopped_acked" };
    const unsigned s = g_mb->d_state;
    printf("PCDSP dsp_log(%s) state=%s heartbeat=%u dsp_ms=%u last_job_ms=%u idle_ms=%u rel_total=%u rel_idle=%u rel_in_job=%u vtcm_held=%u epoch=%u stop_ack=%s rel_n=%u rel_t_ms=%u,%u,%u,%u acq=%d jobs=%u vote: n=%u sleep=%u/%u rc=%d lat_us=%u lpm_read=%u lat2=%u/%d | host: gap_syncs=%ld epoch_syncs=%ld warm_jobs=%ld\n",
           why, s < 5 ? sn[s] : "?", g_mb->d_heartbeat, g_mb->d_now_ms, g_mb->d_last_job_ms, g_mb->d_now_ms - g_mb->d_last_job_ms,
           g_mb->d_rel_total, g_mb->d_rel_idle, g_mb->d_rel_injob, g_mb->d_vtcm_held, g_mb->d_vtcm_epoch, g_mb->d_stop_ack == 0x53544F50u ? "STOP" : "none",
           g_mb->d_rel_n, g_mb->d_rel_t_ms[0], g_mb->d_rel_t_ms[1], g_mb->d_rel_t_ms[2], g_mb->d_rel_t_ms[3], (int)g_mb->d_acq_code, g_mb->jobs,
           g_mb->d_v_n, g_mb->d_v_sleep >> 8, g_mb->d_v_sleep & 255u, (int)g_mb->d_v_dcvs_rc, g_mb->d_v_lat, g_mb->d_lpm_req, g_mb->d_v_lat2, (int)g_mb->d_v_lat2_rc,
           g_gap_syncs, g_epoch_syncs, g_warm_jobs);
    fflush(stdout);
}
static int mb_resync(void);
static int build_stream(void);
static int build_stream(void){
    g_cmd[g_ncmd++] = 0;
    if(v2_on()){
        g_cmd[3] = g_ncmd; g_cmd[4] = g_nst; g_cmd[5] = g_nsi;
        memcpy(g_cmd + g_ncmd, g_st, sizeof(int) * 4 * (size_t)g_nst); g_ncmd += 4 * g_nst;
        memcpy(g_cmd + g_ncmd, g_si, sizeof(int) * 2 * (size_t)g_nsi); g_ncmd += 2 * g_nsi;
        if(g_mirror){
            if(g_dirty_all || g_ncmd + 2 * g_ndirty + 16 > g_cmdcap){ g_cmd[6] = -1; g_full_copies++; }
            else if(!g_pipe){ g_cmd[6] = g_ndirty; memcpy(g_cmd + g_ncmd, g_dirty, sizeof(int) * 2 * (size_t)g_ndirty); g_ncmd += 2 * g_ndirty;
                   for(int i = 0; i < g_ndirty; i++) g_dirty_bytes += g_dirty[2 * i + 1]; }
            else {   /* inline: triples, then the bytes themselves (snapshot of the cache as it is NOW) */
                long need = 3L * g_ndirty; for(int i = 0; i < g_ndirty; i++) need += (g_dirty[2 * i + 1] + 3) / 4;
                if(g_ncmd + need + 64 > g_cmdcap){ g_cmd[6] = -1; g_full_copies++; }
                else {
                    g_cmd[6] = g_ndirty; int tb = g_ncmd; g_ncmd += 3 * g_ndirty;
                    for(int i = 0; i < g_ndirty; i++){
                        const int off = g_dirty[2 * i], nb = g_dirty[2 * i + 1];
                        g_cmd[tb + 3 * i] = off; g_cmd[tb + 3 * i + 1] = nb; g_cmd[tb + 3 * i + 2] = g_ncmd;
                        memcpy(g_cmd + g_ncmd, g_cache + off, (size_t)nb); g_ncmd += (nb + 3) / 4;
                        g_dirty_bytes += nb; g_inline_bytes += nb;
                    }
                }
            }
            g_ndirty = 0; g_dirty_all = 0;              /* the DSP copies them in this call */
            /* thread ranges balanced by pixels */
            const int T = g_threads > 4 ? 4 : g_threads;
            g_cmd[7] = g_ncmd; g_cmd[g_ncmd++] = 0;
            long acc = 0; int j = 0;
            for(int k = 1; k < T; k++){
                const long target = (g_sipix * k) / T;
                while(j < g_nsi && acc + g_sic[j] <= target){ acc += g_sic[j]; j++; }
                g_cmd[g_ncmd++] = j;
            }
            g_cmd[g_ncmd++] = g_nsi;
            if((g_cmd[1] >> 18) & 1){                   /* phase 6: the water spans' positions */
                g_cmd[g_ncmd++] = g_ntu; memcpy(g_cmd + g_ncmd, g_tu, sizeof(int) * (size_t)g_ntu); g_ncmd += g_ntu;
            }
        }
    }
    return 0;
}
void pcdsp_set_turb(int on){ g_turb = on ? 1 : 0; q_reset(); }
void pcdsp_set_turb_sick(int on){ g_turb_sick = on ? 1 : 0; }
void pcdsp_set_turb_hvx(int on){ g_turb_hvx = on ? 1 : 0; q_reset(); }
int pcdsp_turb_on(void){ return !g_dsp_lost && g_cmd && g_open && v2_on() && g_mirror && g_turb && ((g_cmd[1] >> 18) & 1); }
void pcdsp_q_turb(const unsigned char* tex64, const int* turb128){
    if(tex64 != g_turb_tex || turb128 != g_turb_tab) g_turb_texpos = -1;
    g_turb_tex = tex64; g_turb_tab = turb128;
}
int* pcdsp_q_turbspan(const unsigned char* pdest, const short* pz, int count, int izi, int izistep){
    const int need = 7 + 4 * ((count + 15) >> 4);
    if(g_ncmd + need + TU_TEXW + 4 * g_nst + 2 * g_nsi + g_ntu + 2 + 4096 > g_cmdcap || g_ntu >= TU_MAX){
        g_full_flushes++; pcdsp_flush();               /* q_reset: the texture record is written again below */
    }
    if(g_turb_texpos < 0){
        const int p = g_ncmd;
        g_cmd[p] = 6; g_cmd[p + 1] = 0;
        memcpy(g_cmd + p + 2, g_turb_tab, 128 * sizeof(int));
        memcpy(g_cmd + p + 130, g_turb_tex, 4096);
        if(g_turb_sick) ((unsigned char*)(g_cmd + p + 130))[0] ^= 0x55;   /* known-sick: one texel */
        g_ncmd += TU_TEXW; g_turb_texpos = p; g_turb_texrecs++;
    }
    g_tu[g_ntu++] = g_ncmd;
    int* c = g_cmd + g_ncmd;
    c[0] = 5; c[1] = (int)(pdest - g_fb); c[2] = (int)(pz - g_zb); c[3] = count; c[4] = izi; c[5] = izistep; c[6] = g_turb_texpos;
    g_ncmd += need; g_turb_spans++; g_turb_pix += count;
    return c + 7;
}
void pcdsp_flush(void){
    if(g_ncmd <= g_hdr) return;
    if(g_closing || g_dsp_lost){ q_reset(); return; }   /* phase 7: nothing new during the shutdown; phase 9: nor to a lost DSP */
    if(!g_open){ g_errors++; q_reset(); return; }
    if(g_pipe && g_dq && g_async_ok && g_primed && !needs_full() && !mb_resync()){   /* phase 5: a mid-frame flush in a pipelined frame: asynchronous */
        build_stream(); submit_async(); g_async_partial++;
        return;
    }
    if(g_pipe){                                     /* synchronous call through the worker */
        pcdsp_wait();
        build_stream();
        pq_job_h* j = &g_jobs[g_cmdcur]; j->cmd = g_cmd; j->ncmd = g_ncmd; j->fb = g_fb; j->zb = g_zb; j->async = 0;
        for(int i = 0; i < 26; i++) j->st[i] = 0; j->st[0] = -111; j->st[12] = -98; j->st[18] = -1; j->st[22] = j->st[23] = j->st[24] = j->st[25] = -1;
        if(g_dq){ j->t_sub = now_ms(); dq_submit(j); dq_wait(j); } else w_run(job_call, j);
        job_fold(j); g_sync_jobs++; g_primed = 1;
        q_reset();
        return;
    }
    build_stream();
    int st[26] = { -111, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -98, 0, 0, 0, 0, 0, -1, 0, 0, -1, -1, -1, -1, -1 };
    double t0 = now_ms();
    int rc = pxquake_spans(H, g_cmd, g_ncmd, g_cache, g_cachelen, g_fb, g_fblen, g_zb, g_zbn, st, 26);
    if(g_mcheck){ g_mcheck_calls++; g_mcheck_bad += st[20]; if(st[20] && g_mcheck_first < 0) g_mcheck_first = st[21]; }
    if(st[18] >= 0){ g_stage_us += st[18]; g_stage_calls++; }
    g_thvx += st[13]; g_tfb += st[14]; g_zhvx += st[15]; g_zsc += st[16]; g_gsok = st[17];
    g_staged_bytes += st[10]; g_staged_surfs += st[11]; g_vtcm_code = st[12];
    double dt = now_ms() - t0;
    g_calls++; g_frame_calls++; g_rpc_ms_total += dt; g_frame_rpc_ms += dt; g_dsp_us_total += st[4]; g_frame_dsp_us += st[4];
    g_spans_total += st[1]; g_pix_total += st[2];
    if(st[22] >= 0) g_turb_hvx_spans += st[22]; if(st[23] >= 0) g_turb_sc_spans += st[23];   /* phase 6 witness (mode 2) */
    if(st[24] >= 0){ g_ph2_us += st[24]; g_ph_jobs++; } if(st[25] > 0){ g_ph3_us += st[25]; g_ph3_jobs++; }
    if(rc != 0 || st[0] != 0x5A || st[5] != 0 || st[6] != -1 || st[8] != 0){
        g_errors++;
        if(g_errors < 10) dlog("span call FAILED rc=%d st=%d spans=%d pix=%d oob_texel=%d badop=%d oob_dest=%d", rc, st[0], st[1], st[2], st[5], st[6], st[8]);
    }
    q_reset();
}
void pcdsp_q_surface(const unsigned char* texels, int cachewidth, const unsigned char* blo, const unsigned char* bhi){
    g_cur_tex = texels; g_cur_cw = cachewidth; g_cur_blo = blo; g_cur_bhi = bhi;
    if(v2_on()){
        const int off = (int)(texels - g_cache); int nb = (int)(bhi - texels); if(nb > 65536) nb = 65536;
        int found = -1;
        for(int i = g_nst - 1; i >= 0; i--) if(g_st[4 * i] == off && g_st[4 * i + 1] == nb && g_st[4 * i + 3] == cachewidth){ found = i; break; }
        if(found < 0){
            unsigned vo = g_mirror ? (unsigned)off : ((g_vused + 127u) & ~127u);
            if((!g_mirror && vo + (unsigned)nb > V2_VBUDGET) || g_nst >= V2_MAXS || g_ncmd + 4 * (g_nst + 1) + 2 * g_nsi + 4096 > g_cmdcap){
                g_full_flushes++; pcdsp_flush(); vo = g_mirror ? (unsigned)off : 0; }
            found = g_nst++;
            g_st[4 * found] = off; g_st[4 * found + 1] = nb; g_st[4 * found + 2] = (int)vo; g_st[4 * found + 3] = cachewidth;
            g_vused = vo + (unsigned)nb;
        }
        g_cursurf = found;
    }
    if(g_ncmd + 3 + 64 > g_cmdcap){ g_full_flushes++; pcdsp_flush(); }
    if(g_nblk >= MAXBLK){ g_full_flushes++; pcdsp_flush(); }
    g_blo[g_nblk] = blo; g_bhi[g_nblk] = bhi; g_nblk++;
    g_cmd[g_ncmd++] = g_vtcm ? 4 : 1; g_cmd[g_ncmd++] = (int)(texels - g_cache); g_cmd[g_ncmd++] = cachewidth;
    if(g_vtcm) g_cmd[g_ncmd++] = (int)(bhi - texels);
}
static int g_cur_surf[4];
int* pcdsp_q_span(const unsigned char* pdest, const short* pz, int count, int izi, int izistep){
    int need = 6 + 4 * ((count + 7) >> 3);
    if(v2_on()){
        if(g_ncmd + need + 4 + 4 * g_nst + 2 * (g_nsi + 1) + 64 > g_cmdcap || g_nsi >= V2_MAXI){
            g_full_flushes++; pcdsp_flush();
            pcdsp_q_surface(g_cur_tex, g_cur_cw, g_cur_blo, g_cur_bhi);     /* the current surface again */
        }
        g_si[2 * g_nsi] = g_ncmd; g_si[2 * g_nsi + 1] = g_cursurf; g_sic[g_nsi] = count + 128; g_sipix += count + 128; g_nsi++;   /* cost = pixels + a per-span overhead */
        int* c = g_cmd + g_ncmd;
        c[0] = 2; c[1] = (int)(pdest - g_fb); c[2] = (int)(pz - g_zb); c[3] = count; c[4] = izi; c[5] = izistep;
        g_ncmd += need;
        return c + 6;
    }
    if(g_ncmd + need + 4 > g_cmdcap){
        /* re-emit the current surface after the flush */
        int k = g_ncmd - 1; (void)k;
        g_full_flushes++;
        int i = 2, last = -1;
        while(i < g_ncmd){ if(g_cmd[i] == 1){ last = i; i += 3; } else if(g_cmd[i] == 4){ last = i; i += 4; } else { i += 6 + 4 * ((g_cmd[i + 3] + 7) >> 3); } }
        int sl = (last >= 0 && g_cmd[last] == 4) ? 4 : 3;
        if(last >= 0){ for(int q = 0; q < sl; q++) g_cur_surf[q] = g_cmd[last + q]; }
        const unsigned char* blo = g_nblk ? g_blo[g_nblk - 1] : NULL; const unsigned char* bhi = g_nblk ? g_bhi[g_nblk - 1] : NULL;
        pcdsp_flush();
        if(last >= 0){ for(int q = 0; q < sl; q++) g_cmd[g_ncmd++] = g_cur_surf[q];
                       g_blo[g_nblk] = blo; g_bhi[g_nblk] = bhi; g_nblk++; }
    }
    int* c = g_cmd + g_ncmd;
    c[0] = 2; c[1] = (int)(pdest - g_fb); c[2] = (int)(pz - g_zb); c[3] = count; c[4] = izi; c[5] = izistep;
    g_ncmd += need;
    return c + 6;
}
/* called by pc/d_dspspans.c after the segments of the first span of a frame are written */
int pcdsp_sick_now(void);
int pcdsp_sick_now(void){
    if(g_sick_from < 0 || !g_first_span_of_frame) return 0;
    g_first_span_of_frame = 0;
    return g_frames >= g_sick_from;
}
int pcdsp_is_queued(const unsigned char* lo, const unsigned char* hi){
    for(int i = 0; i < g_nblk; i++) if(lo < g_bhi[i] && g_blo[i] < hi) return 1;
    return 0;
}
void pcdsp_guard(const unsigned char* lo, const unsigned char* hi){
    for(int i = 0; i < g_nblk; i++)
        if(lo < g_bhi[i] && g_blo[i] < hi){ g_guard_flushes++; pcdsp_flush(); return; }
}
/* pipeline: launch the queued spans as an ASYNCHRONOUS job (at most one in flight; the CPU fills the other stream) */
void pcdsp_launch(void){
    if(g_ncmd <= g_hdr) return;
    if(g_closing || g_dsp_lost){ q_reset(); return; }
    if(!g_open){ g_errors++; q_reset(); return; }
    if(!g_primed){ pcdsp_flush(); return; }          /* the first call copies the whole cache: never concurrently */
    if(needs_full() || mb_resync()){ g_sync_full++; const int a = g_async_ok; g_async_ok = 0; pcdsp_flush(); g_async_ok = a; return; }  /* a FULL copy (or a gap, phase 7): synchronous */
    if(!g_dq) pcdsp_wait();                          /* the worker mode keeps one job in flight */
    build_stream();
    submit_async();
}
void pcdsp_note_wait(double ms){ if(g_nfw < 20000) g_frame_wait[g_nfw++] = (float)ms; }
/* pipeline frame end: count the frame (the job's own statistics arrive when it is waited for) */
void pcdsp_frame_end_async(void){
    pcdsp_launch();
    if(g_nf < MAXF){ g_frame_ms[g_nf] = (float)g_frame_rpc_ms; g_frame_us[g_nf] = (float)g_frame_dsp_us; g_nf++; }
    g_frames++; g_frame_rpc_ms = 0; g_frame_dsp_us = 0; g_frame_calls = 0; g_first_span_of_frame = 1;
}
void pcdsp_frame_end(void){
    pcdsp_flush();
    if(g_nf < MAXF){ g_frame_ms[g_nf] = (float)g_frame_rpc_ms; g_frame_us[g_nf] = (float)g_frame_dsp_us; g_nf++; }
    g_frames++; g_frame_rpc_ms = 0; g_frame_dsp_us = 0; g_frame_calls = 0; g_first_span_of_frame = 1;
}

/* ---------------------------------------------------------------- call-cost bench */
int pcdsp_bench(int n, int w, int h){
    int bad = 0;
    if(n < 10) n = 10;
    if(pcdsp_open() != 0){ printf("PCDSP bench error=open %s\n", g_err); return 2; }
    const int fbn = w * h, cachen = (int)(1312.0 * 1024.0 * ((double)fbn / (640.0 * 480.0)));
    unsigned char* fb = (unsigned char*)pcdsp_alloc(fbn);
    short* zb = (short*)pcdsp_alloc(fbn * 2);
    unsigned char* cache = (unsigned char*)pcdsp_alloc(cachen);
    unsigned char* heap = (unsigned char*)malloc((size_t)fbn);
    int* cmd = (int*)pcdsp_alloc(4 << 20);                 /* 4 MB: the stream-size test below */
    float* t = (float*)malloc(sizeof(float) * (size_t)n);
    if(!fb || !zb || !cache || !heap || !cmd || !t){ printf("PCDSP bench error=alloc %s\n", g_err); pcdsp_close(); return 2; }
    memset(heap, 0x3C, (size_t)fbn); fb[0] = 0x12; fb[fbn - 1] = 0x34;
    int st[9];
    printf("PCDSP bench w=%d h=%d n=%d fb=%d zb=%d cache=%d\n", w, h, n, fbn, fbn * 2, cachen);
    for(int i = 0; i < 50; i++) pxquake_nop(H);                       /* warm-up: clock vote, mappings */
    for(int i = 0; i < n; i++){ double a = now_ms(); int rc = pxquake_nop(H); t[i] = (float)(now_ms() - a); if(rc) bad++; }
    stat_line("nop_ms", t, n);
    for(int i = 0; i < 20; i++) pxquake_touch(H, fb, fbn, st, 4);
    for(int i = 0; i < n; i++){ double a = now_ms(); int rc = pxquake_touch(H, fb, fbn, st, 4); t[i] = (float)(now_ms() - a);
        if(rc || st[0] != 0x51 || st[1] != 0x12 || st[2] != 0x34 || st[3] != fbn) bad++; }
    stat_line("touch_rpcmem_frame_ms", t, n);
    for(int i = 0; i < 20; i++) pxquake_touch(H, heap, fbn, st, 4);
    for(int i = 0; i < n; i++){ double a = now_ms(); int rc = pxquake_touch(H, heap, fbn, st, 4); t[i] = (float)(now_ms() - a);
        if(rc || st[0] != 0x51 || st[1] != 0x3C || st[3] != fbn) bad++; }
    stat_line("touch_heap_copy_frame_ms", t, n);
    { /* DSP-side memory probe on the shared frame buffer (5 rounds) */
        int ps[10];
        for(int r = 0; r < 5; r++){
            for(int i = 0; i < fbn; i++) fb[i] = (unsigned char)(i * 5 + r); fb[0] = 0x77;
            memset(ps, 0, sizeof ps);
            int rc = pxquake_touch(H, fb, fbn, ps, 10);
            printf("PCDSP memprobe bytes=%d rc=%d sum_shared_us=%d memcpy_in_us=%d sum_local_us=%d strided64_shared_us=%d sums_agree=%d memcpy_out_us=%d\n",
                   fbn, rc, ps[4], ps[5], ps[6], ps[7], ps[8], ps[9]);
        }
        fb[0] = 0x12; fb[fbn - 1] = 0x34;
    }
    /* the real per-frame call with everything a frame hands over, but an empty command stream */
    cmd[0] = 0;
    for(int i = 0; i < 20; i++) pxquake_spans(H, cmd, 1, cache, cachen, fb, fbn, zb, fbn, st, 9);
    for(int i = 0; i < n; i++){ double a = now_ms(); int rc = pxquake_spans(H, cmd, 1, cache, cachen, fb, fbn, zb, fbn, st, 9); t[i] = (float)(now_ms() - a);
        if(rc || st[0] != 0x5A || st[1] != 0) bad++; }
    stat_line("spans_empty_allbufs_ms", t, n);
    {   /* stream size: 64 KB .. 4 MB of int32, all written by the CPU before each call (as the real queue is) */
        static const int kb[6] = { 64, 256, 640, 1280, 2560, 4096 };
        for(int z = 0; z < 6; z++){
            const int nw = kb[z] * 256;
            for(int i = 0; i < 10; i++){ for(int k = 0; k < nw; k++) cmd[k] = k; cmd[0] = 0; pxquake_spans(H, cmd, nw, cache, cachen, fb, fbn, zb, fbn, st, 9); }
            for(int i = 0; i < n / 4; i++){
                for(int k = 0; k < nw; k += 16) cmd[k] = k;      /* dirty every cache line, as the queue does */
                cmd[0] = 0;
                double a = now_ms(); int rc = pxquake_spans(H, cmd, nw, cache, cachen, fb, fbn, zb, fbn, st, 9); t[i] = (float)(now_ms() - a);
                if(rc || st[0] != 0x5A) bad++;
            }
            char nm[64]; snprintf(nm, sizeof nm, "spans_stream_%dKB_ms", kb[z]); stat_line(nm, t, n / 4);
        }
        cmd[0] = 0;
    }
    /* coherency: the CPU dirties fb/zb, the DSP writes short spans that share cache lines with CPU pixels; every byte
     * must come out as either the CPU's or the DSP's expected value. 20 rounds with different patterns. */
    int mism = 0, mismz = 0;
    for(int r = 0; r < 20; r++){
        for(int i = 0; i < cachen; i++) cache[i] = (unsigned char)(i * 13 + 5 + r);
        for(int i = 0; i < fbn; i++) fb[i] = (unsigned char)(i * 7 + r);
        for(int i = 0; i < fbn; i++) zb[i] = (short)(i * 3 + r);
        int k = 0;
        cmd[k++] = 1; cmd[k++] = 0; cmd[k++] = 64;
        for(int y = r % 3; y < h; y += 3){
            int u = (y % 13) + 1, cnt = 5 + (y % 11);
            cmd[k++] = 2; cmd[k++] = y * w + u; cmd[k++] = y * w + u; cmd[k++] = cnt; cmd[k++] = (y + 1) << 16; cmd[k++] = 1 << 16;
            for(int s = 0; s < (cnt + 7) / 8; s++){ cmd[k++] = (s * 8) << 16; cmd[k++] = (y & 63) << 16; cmd[k++] = 1 << 16; cmd[k++] = 0; }
        }
        cmd[k++] = 0;
        int rc = pxquake_spans(H, cmd, k, cache, cachen, fb, fbn, zb, fbn, st, 9);
        if(rc || st[0] != 0x5A || st[5] || st[8]) bad++;
        for(int y = 0; y < h; y++) for(int x = 0; x < w; x++){
            int i = y * w + x; unsigned char ef = (unsigned char)(i * 7 + r); short ez = (short)(i * 3 + r);
            if((y % 3) == (r % 3)){ int u = (y % 13) + 1, cnt = 5 + (y % 11);
                if(x >= u && x < u + cnt){ int px = x - u; ef = cache[px + (y & 63) * 64]; ez = (short)(y + 1 + px); } }
            if(fb[i] != ef) mism++;
            if(zb[i] != ez) mismz++;
        }
    }
    printf("PCDSP coherency rounds=20 fb_mismatch=%d zb_mismatch=%d %s\n", mism, mismz, (mism || mismz) ? "FAILED" : "ok");
    if(mism || mismz) bad++;
    printf("PCDSP bench bad=%d\n", bad);
    free(t); free(heap);
    pcdsp_close();
    return bad ? 1 : 0;
}
