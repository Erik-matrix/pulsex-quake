/* pq_queue.h - phase 5 (2026-10-26): the persistent frame worker of pxquake on the DSP.
 * Included at the END of pxquake_imp.c (uses pxquake_spans and the context).
 *
 * The host creates a dspqueue (Hexagon SDK; the same transport ggml-hexagon uses) and hands its id to q_start. A DSP
 * thread imports it and WAITS in dspqueue_read (100 ms timeout, then it checks its stop flag): no busy polling, the
 * thread sleeps until a packet arrives. Packets:
 *   PING  {type 1, seq}                                   -> answered at once (latency measurement)
 *   FRAME {type 2, seq, lengths} + 4 buffers (stream, cache, fb, zb), mapped once by the host (fastrpc_mmap)
 *          -> pxquake_spans(...) exactly as the FastRPC call does -> response {type, seq, status[24]} + fb, zb
 *   STOP  {type 3}                                        -> answered, the thread ends
 * q_stop sets the flag, joins the thread (it ends within the 100 ms timeout even without a STOP packet) and closes
 * the queue; pxquake_close calls it too, so a session close can never leave the thread running.
 * Cache maintenance: the response's fb/zb references carry FLUSH_SENDER (the DSP writes its cache lines back before
 * the host is told), unless the frame request asked otherwise (rq.flags bit 0 = no flush, for the measurement). */
#include "dspqueue.h"

typedef struct { uint32_t type, seq; int cmdsLen, cacheLen, fbLen, zbLen; uint32_t flags; uint32_t pad; } pq_qreq;
typedef struct { uint32_t type, seq; int st[24]; } pq_qrsp;

static dspqueue_t g_q = NULL;
static qurt_thread_t g_qt;
static volatile int g_qstop = 0, g_qrun = 0;
static pq_ctx* g_qctx = NULL;
static long long g_qframes = 0, g_qpings = 0;
static char g_qstk[65536] __attribute__((aligned(128)));
static qurt_signal_t g_qsig; static int g_qsig_ok = 0;
static volatile int g_qerr = 0; static long long g_qwakes = 0;
static void pq_qcb(dspqueue_t q, int err, void* ctx){ (void)q; (void)err; (void)ctx; qurt_signal_set(&g_qsig, 1u); }
static void pq_qerrcb(dspqueue_t q, int err, void* ctx){ (void)q; (void)ctx; g_qerr = err; qurt_signal_set(&g_qsig, 1u); }

static void pq_qthread(void* arg){
    (void)arg;
    int stop = 0;
    while(!g_qstop && !stop){
        qurt_signal_wait(&g_qsig, 1u, QURT_SIGNAL_ATTR_WAIT_ANY);    /* sleeps until a packet (or stop) */
        qurt_signal_clear(&g_qsig, 1u);
        g_qwakes++;
        for(;;){
        if(g_qstop) break;
        uint32_t flags = 0, nb = 0, len = 0;
        struct dspqueue_buffer b[4];
        pq_qreq rq;
        int err = dspqueue_read_noblock(g_q, &flags, 4, &nb, b, sizeof rq, &len, (uint8_t*)&rq);
        if(err == AEE_EWOULDBLOCK) break;
        if(err){ stop = 1; break; }
        pq_qrsp rs; pq_zero(&rs, sizeof rs); rs.type = rq.type; rs.seq = rq.seq;
        if(len < 8){ continue; }
        if(rq.type == 3){ dspqueue_write(g_q, DSPQUEUE_PACKET_FLAG_MESSAGE, 0, NULL, sizeof rs, (const uint8_t*)&rs, DSPQUEUE_TIMEOUT_NONE); stop = 1; break; }
        if(rq.type == 2 && nb == 4 && len >= sizeof rq){
            rs.st[0] = -111;
            pxquake_spans((remote_handle64)(uintptr_t)g_qctx, (const int*)b[0].ptr, rq.cmdsLen, (const unsigned char*)b[1].ptr, rq.cacheLen,
                          (unsigned char*)b[2].ptr, rq.fbLen, (short*)b[3].ptr, rq.zbLen, rs.st, 22);
            g_qframes++;
            struct dspqueue_buffer ob[2]; ob[0] = b[2]; ob[1] = b[3];
            const uint32_t fl = (rq.flags & 1) ? 0 : (DSPQUEUE_BUFFER_FLAG_FLUSH_SENDER | DSPQUEUE_BUFFER_FLAG_INVALIDATE_RECIPIENT);
            ob[0].flags = fl; ob[1].flags = fl;
            dspqueue_write(g_q, DSPQUEUE_PACKET_FLAG_MESSAGE | DSPQUEUE_PACKET_FLAG_BUFFERS, 2, ob, sizeof rs, (const uint8_t*)&rs, DSPQUEUE_TIMEOUT_NONE);
            continue;
        }
        g_qpings++;
        rs.st[0] = 0x70; rs.st[1] = (int)nb;
        dspqueue_write(g_q, DSPQUEUE_PACKET_FLAG_MESSAGE, 0, NULL, sizeof rs, (const uint8_t*)&rs, DSPQUEUE_TIMEOUT_NONE);
        }
    }
    g_qrun = 0;
}

static int pq_qstop_internal(void){
    int js = -1;
    if(g_qrun || g_qt){
        g_qstop = 1;
        if(g_qsig_ok) qurt_signal_set(&g_qsig, 1u);
        if(g_qt){ qurt_thread_join((unsigned)g_qt, &js); g_qt = 0; }
    }
    if(g_q){ dspqueue_close(g_q); g_q = NULL; }
    if(g_qsig_ok){ qurt_signal_destroy(&g_qsig); g_qsig_ok = 0; }
    g_qrun = 0;
    return js;
}

int pxquake_q_start(remote_handle64 h, int id_lo, int id_hi, int* status, int statusLen){
    pq_ctx* c = pq_get(h);
    if(!c || statusLen < 4) return AEE_EBADPARM;
    if(g_q){ status[0] = -1; return AEE_SUCCESS; }
    const uint64_t qid = (uint64_t)(uint32_t)id_lo | ((uint64_t)(uint32_t)id_hi << 32);
    qurt_signal_init(&g_qsig); g_qsig_ok = 1;
    int err = dspqueue_import(qid, pq_qcb, pq_qerrcb, NULL, &g_q);
    status[1] = err;
    if(err){ g_q = NULL; qurt_signal_destroy(&g_qsig); g_qsig_ok = 0; status[0] = -2; return AEE_SUCCESS; }
    g_qctx = c; g_qstop = 0; g_qrun = 1;
    const int prio = qurt_thread_get_priority(qurt_thread_get_id());
    qurt_thread_attr_t at; qurt_thread_attr_init(&at);
    qurt_thread_attr_set_stack_addr(&at, g_qstk); qurt_thread_attr_set_stack_size(&at, sizeof g_qstk);
    qurt_thread_attr_set_priority(&at, (unsigned short)(prio > 1 ? prio : 100));
    if(qurt_thread_create(&g_qt, &at, pq_qthread, NULL) != 0){ g_qt = 0; g_qrun = 0; dspqueue_close(g_q); g_q = NULL; qurt_signal_destroy(&g_qsig); g_qsig_ok = 0; status[0] = -3; return AEE_SUCCESS; }
    status[0] = 0x71; status[2] = prio; status[3] = 0;
    return AEE_SUCCESS;
}
int pxquake_q_stop(remote_handle64 h, int* status, int statusLen){
    (void)h;
    const long long fr = g_qframes, pg = g_qpings, wk = g_qwakes;
    const int js = pq_qstop_internal();
    if(statusLen > 0) status[0] = 0x72;
    if(statusLen > 1) status[1] = js;
    if(statusLen > 2) status[2] = (int)fr;
    if(statusLen > 3) status[3] = (int)pg;
    if(statusLen > 4) status[4] = (int)wk;
    if(statusLen > 5) status[5] = g_qerr;
    return AEE_SUCCESS;
}
