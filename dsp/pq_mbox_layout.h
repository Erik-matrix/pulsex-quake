/* pq_mbox_layout.h - phase 5: the mailbox page shared by the host (bridge/pxquake/pc_dsp.c) and the DSP (pq_mbox.h).
 * Lines written by the HOST and lines written by the DSP never share a 128-byte block: the DSP writes back whole cache
 * lines, and a line holding both sides' fields would let the DSP overwrite a request the host had just posted.
 * Ring of MB_SLOTS slots (pipeline depth): request seq lives in slot[seq % MB_SLOTS]. */
#ifndef PQ_MBOX_LAYOUT_H
#define PQ_MBOX_LAYOUT_H
#include <stdint.h>
#define MB_SLOTS 4
typedef struct {
    /* request: written by the host */
    int fd_cmds, fd_cache, fd_fb, fd_zb;
    int off_cmds, off_fb, off_zb, flags;            /* flags bit 0: the DSP invalidates the shared cache (it may read it) */
    int cmdsLen, cacheLen, fbLen, zbLen;
    int req_pad[20];                                /* -> 128 bytes */
    /* answer: written by the DSP */
    int st[26];                                     /* phase 6: [24] us of the world spans, [25] us of the water */
    int dsp_us_inv, dsp_us_job, dsp_us_flush, ans_pad[3];   /* -> 128 bytes */
} pq_slot;
typedef struct {
    /* written by the host */
    volatile uint32_t req_seq, stop, poll_us, spin_us;
    volatile uint32_t test_flags;                   /* TEST bit 0: the DSP thread returns without qurt_thread_exit; bit 1: the old flow (stop early, no linger); bit 2: the HVX workers skip qurt_thread_exit; bit 3: the thread ends 1.5 s late (bounded-stop test); bit 4: the stop ack comes 5 s late; bit 6: no answers after 300 jobs */
    volatile uint32_t lpm;                          /* phase 7: sleep_disable for the DCVS vote (0 = default, 1 = no LPM) */
    volatile uint32_t lat;                          /* phase 7: lever 2, a DCVS latency vote in us on its own context (0 = off) */
    volatile uint32_t spinidle;                     /* phase 7: 1 = the DSP thread never sleeps while idle (spins) */
    volatile uint32_t clock;                        /* phase 9: the clock vote, 0 turbo (default) 1 nom 2 dcvs */
    volatile uint32_t recycle;                      /* phase 9: bit 0 = this stop is a recycle; bit 1 = also join the HVX workers */
    uint32_t host_pad[22];                          /* -> 128 bytes */
    /* written by the DSP */
    volatile uint32_t rsp_seq, alive, jobs, idle_sleeps, polls_lo, polls_hi;
    /* phase 7: the DSP's idle log - readable by the host without an RPC (the line is flushed while polling) */
    volatile uint32_t d_state;                      /* 1 polling, 2 in a job, 3 idle-sleeping, 4 stopped + acked */
    volatile uint32_t d_heartbeat, d_now_ms, d_last_job_ms;   /* DSP clock, ms */
    volatile uint32_t d_rel_total, d_rel_idle, d_rel_injob;  /* VTCM release requests; honoured while idle; met by a job */
    volatile uint32_t d_vtcm_held, d_vtcm_epoch, d_stop_ack;  /* epoch +1 per VTCM release; ack = 0x53544F50 "STOP" */
    volatile uint32_t d_rel_n, d_rel_t_ms[4];       /* the last four release requests (DSP ms) */
    volatile int32_t d_acq_code;
    /* phase 7: what the power vote APPLIED (the witness of -dsplpm / -dsplat) */
    volatile uint32_t d_v_n, d_v_sleep, d_v_lat, d_v_ms;    /* votes made; (set << 8) | sleep_disable; DCVS latency us; DSP ms */
    volatile int32_t d_v_dcvs_rc;
    volatile uint32_t d_lpm_req, d_v_lat2;          /* what the DSP read from the host line; lever 2 applied (us) */
    volatile int32_t d_v_lat2_rc;
    volatile uint32_t d_v_clock;                    /* phase 9: the clock vote applied: mode | 0x100 DCVS enabled (HAP_power_get) | 0x8000 read failed */
    volatile uint32_t d_clk_mhz;                    /* phase 9: core clock now (HAP_power_get_clk_Freq), every 1000 jobs */
    pq_slot slot[MB_SLOTS];
    /* phase 9: DSP-written resource counters, every 1000 jobs (see pq_mbox.h mb_res) */
    volatile uint32_t r_jobs, r_heap_free_kb, r_heap_used_kb, r_seg_free, r_seg_used, r_mem_rc, r_stack_mb, r_stack_wk;
    volatile uint32_t r_vtcm_acq, r_votes, r_forks, r_cacheops_k, r_dsp_ms, r_maxjob_us, r_nmb, r_calls;
    volatile uint32_t r_ring_used_kb[16];          /* heap used, one sample per 1000 jobs: [(jobs / 1000) & 15] */
} pq_mb;
/* phase 7 (2026-10-27): a miscounted pad once shifted the slots by 4 bytes - the DSP's line flush then overwrote the
 * host's request line (-112, broken frames). The layout is now checked at compile time on both sides. */
#include <stddef.h>
_Static_assert(sizeof(pq_slot) == 256, "pq_slot must be two 128-byte lines");
_Static_assert(offsetof(pq_mb, rsp_seq) == 128, "the DSP line must start at 128");
_Static_assert(offsetof(pq_mb, slot) == 256, "the slots must start at 256");
_Static_assert(offsetof(pq_slot, st) == 128, "the answer line must start at 128");
_Static_assert(offsetof(pq_mb, r_jobs) == 256 + 4 * 256, "the resource line must follow the slots");
_Static_assert(sizeof(pq_mb) == 256 + 4 * 256 + 128, "pq_mb must be the mailbox lines exactly");
#endif
