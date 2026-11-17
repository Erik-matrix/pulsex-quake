/* pc_dsp.h - host bridge of PulseX Quake phase 2 (span drawing on the Hexagon cDSP). Plain C, no SDK types, so
 * Quake's own headers (byte, qboolean ...) never meet the Hexagon SDK's. Implementation: pc_dsp.c. */
#ifndef PC_DSP_H
#define PC_DSP_H
#ifdef __cplusplus
extern "C" {
#endif

int   pcdsp_open (void);                 /* 0 = session open; idempotent */
/* phase 7: the window procedure brackets itself with these; a DSP RPC in between is counted and fails the run */
void  pcdsp_msg_enter (void);
void  pcdsp_msg_leave (void);
int   pcdsp_lost (void);                 /* phase 9: 1 = the DSP stopped answering; the game renders on the CPU */
void  pcdsp_set_test_lost (int frame, int noack);   /* known-sick: -pctest_dsplost N [-pctest_dsplost_noack] */
int   pcdsp_exit_code (void);            /* 0, 7 = a DSP thread did not end cleanly, 8 = a bounded shutdown wait expired, 9 = an RPC in a window message */
void  pcdsp_set_recycle (int ms);        /* phase 9: -dsprecycle N (s, float), recycle the DSP mailbox thread + VTCM + workers every N s (0 = off) */
void  pcdsp_set_clock (int m);           /* phase 9: -dspclock turbo|nom|dcvs (0|1|2), the DSP clock vote */
void  pcdsp_set_lpm (int n);             /* phase 7: -dsplpm, DCVS sleep_disable (1 = no DSP low-power mode) */
void  pcdsp_set_spinidle (int on);       /* phase 7: -dspqspinidle, the DSP mailbox thread spins instead of sleeping while idle */
void  pcdsp_set_lat (int us);            /* phase 7: -dsplat, lever 2: a mips_bw wakeup-latency vote (0 = off) */
int   pcdsp_lpm_ok (void);               /* 0 = the DSP did not apply what -dsplpm/-dsplat asked (exit code 10) */
void  pcdsp_set_keepwarm (int ms);       /* phase 7: -dspqwarm, an empty mailbox job every ms while idle (0 = off) */
void  pcdsp_tick (void);                 /* phase 7: once per host frame (the keep-warm) */
void  pcdsp_set_dialog (int on);         /* 0 = a stuck shutdown is only logged (tests); 1 = also a dialog for the player */
void  pcdsp_close (void);                /* idempotent; release -> buffers freed -> FASTRPC_SESSION_CLOSE -> handle close */
int   pcdsp_is_open (void);
const char *pcdsp_error (void);

/* shared (rpcmem, registered = zero-copy) memory; freed by pcdsp_close. NULL on failure. */
void *pcdsp_alloc (int bytes);

/* the three frame buffers the span call hands over (all from pcdsp_alloc) */
void  pcdsp_set_buffers (unsigned char *fb, int fblen, short *zb, int zbshorts, unsigned char *cache, int cachelen);

/* the frame's span queue (written by pc/d_dspspans.c) */
void  pcdsp_q_surface (const unsigned char *texels, int cachewidth, const unsigned char *blk_lo, const unsigned char *blk_hi);
int  *pcdsp_q_span (const unsigned char *pdest, const short *pz, int count, int izi, int izistep);  /* -> 4*ceil(count/8) ints to fill */
int   pcdsp_is_queued (const unsigned char *lo, const unsigned char *hi); /* 1 = queued spans read these bytes */
void  pcdsp_guard (const unsigned char *lo, const unsigned char *hi);   /* surface cache bytes about to be overwritten */
void  pcdsp_flush (void);                /* draw everything queued (one FastRPC call) */
void  pcdsp_frame_end (void);            /* end of the world-surface pass: flush + per-frame statistics */
void  pcdsp_set_sick (int from_frame);
/* phase 6: the water (Turbulent8 + z) in the same stream; written by pc/d_dspspans.c PC_DspQueueTurb */
void  pcdsp_set_turb (int on);           /* 0 = the CPU water path */
void  pcdsp_set_turb_sick (int on);      /* known-sick: texel 0 of every inline water texture changed */
void  pcdsp_set_turb_hvx (int on);       /* 0 = the DSP draws the water with the scalar loop */
int   pcdsp_turb_on (void);              /* 1 = the water goes to the DSP now (HVX threaded mirror stream) */
void  pcdsp_q_turb (const unsigned char *tex64, const int *turb128);
int  *pcdsp_q_turbspan (const unsigned char *pdest, const short *pz, int count, int izi, int izistep);  /* -> 4*ceil(count/16) ints */
void  pcdsp_set_hvx (int on);
void  pcdsp_set_mirror (int on);       /* HVX mode: 1 = surface cache mirrored in VTCM (default), 0 = staged per call */
void  pcdsp_dirty (const unsigned char *lo, const unsigned char *hi);   /* surface-cache bytes the CPU rewrote */          /* 1 = HVX kernels (VTCM staging, 4 threads by default) */
void  pcdsp_set_diag (int d);          /* DIAGNOSTIC timing only: bit 0 skip texture, bit 1 skip z (threaded path) */
void  pcdsp_set_vtcm (int on);         /* 1 = stage the frame's surface blocks into VTCM on the DSP (op 4) */
void  pcdsp_set_threads (int n);       /* 0 = serial DSP path (default), 1..6 = threaded path */   /* known-sick control: perturb the first span of every frame from this frame on */

/* call-cost measurement: n calls of each kind, prints PCDSP lines; returns 0 when every check held */
int   pcdsp_bench (int n, int w, int h);

/* phase 4, the pipeline (-dspspans 3): a worker thread owns the session; set before pcdsp_open */
void  pcdsp_set_pipe (int on);
void  pcdsp_set_dq (int on);
void  pcdsp_set_join_test (int f);      /* TEST bit 0: the DSP mailbox thread ends without qurt_thread_exit */            /* phase 5: the persistent DSP worker over a dspqueue (implies the pipeline) */
void  pcdsp_set_dq_noflush (int on);    /* measurement: no cache maintenance flags */
int   pcdsp_dqbench (int n, int w, int h);
void  pcdsp_set_mb_timing (int spin_us, int poll_us);   /* the DSP mailbox thread: spin after a job, sleep while idle */
int   pcdsp_pipe (void);
void  pcdsp_set_target (unsigned char *fb, short *zb);   /* where the next queued spans are drawn (queue empty) */
void  pcdsp_launch (void);
void  pcdsp_set_async_ok (int on);      /* phase 5: mid-frame flushes may be asynchronous (pipelined frame, world pass) */              /* the queued spans as an asynchronous job */
double pcdsp_wait (void);               /* wait for the job in flight; returns the ms waited */
int   pcdsp_inflight (void);
unsigned pcdsp_last_seq (void);       /* host sequence number of the last job submitted */
double pcdsp_wait_upto (unsigned s);   /* wait for the jobs up to that number only */
int   pcdsp_queued (void);             /* spans are queued */
void  pcdsp_frame_end_async (void);     /* end of the world pass in a pipelined frame: launch, count */
void  pcdsp_note_wait (double ms);
void  pcdsp_dirty_all (void);           /* the whole surface cache changed (flush, init) */
void  pcdsp_set_mirrorcheck (int on);
void  pcdsp_test_nohdr (int on);        /* known-sick control: header reports dropped */   /* test: the DSP compares mirror and cache (synchronous calls) */

#ifdef __cplusplus
}
#endif
#endif
