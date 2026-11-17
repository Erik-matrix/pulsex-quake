/* pq_tex3.h - HVX texture, version 3 (2026-10-26, phase 3): the gather pipeline runs ACROSS spans.
 * Measured in hexagon-sim (pq_simbench.c): version 2 cost ~550 cycles per span almost independent of its length
 * (16 px: 559, 130 px: 608) - the latency of one span's chain (deal -> indices -> shuffles -> gather -> load -> store)
 * and of the out-of-block reduction (vextract) was exposed span after span. Version 3:
 *   - a chunk's gathers are issued, and only THEN is the previous chunk (often of the previous span) stored;
 *   - the out-of-block reduction of a span is read only when its first chunk is about to be stored, i.e. one chunk
 *     later; a bad span is redrawn by the scalar loop at that point and its chunks are never stored.
 * Gathers read VTCM only and write only the thread's gather targets, so issuing them for a span that will turn out
 * bad changes nothing. Direct odd offsets (proven exact in hexagon-sim and on the NPU, 4 trees).
 * Call pq_pipe_init before a thread's spans and pq_pipe_end after them. */
#ifndef PQ_TEX3_H
#define PQ_TEX3_H

typedef struct {
    const int* seg; int count; unsigned char* pdest; int base_off, cw; const unsigned char* pv; int vbytes;
    HVX_Vector bad; int decided, isbad;
} pq_sp;
typedef struct {
    pq_sp sp[2]; int cur;
    int pend_active, pend_sp, pend_n, pend_slot, pend_two; unsigned char* pend_dst;
    int chunk;
    int fallbacks, oob;
} pq_pipe;

static inline void pq_pipe_init(pq_pipe* P){ P->cur = 0; P->pend_active = 0; P->chunk = 0; P->fallbacks = 0; P->oob = 0; }

static inline void pq_pipe_finish(const pq_bufs* B, pq_pipe* P){
    if(!P->pend_active) return;
    pq_sp* S = &P->sp[P->pend_sp];
    if(!S->decided){
        S->decided = 1; S->isbad = pq_any(Q6_V_vand_VV(S->bad, Q6_V_vsplat_R((int)0x80000000)));
        if(S->isbad){ P->fallbacks++; pq_tex_scalar(B, S->seg, S->count, S->pdest, S->base_off, S->cw, S->pv, S->vbytes, &P->oob); }
    }
    if(!S->isbad){
        const HVX_Vector* pt = (const HVX_Vector*)B->gs + 2 * P->pend_slot;
        const HVX_Vector G0 = pt[0], G1 = P->pend_two ? pt[1] : Q6_V_vzero();
        pq_st(P->pend_dst, Q6_Vb_vpacke_VhVh(G1, G0), P->pend_n);
    }
    P->pend_active = 0;
}
static inline void pq_pipe_end(const pq_bufs* B, pq_pipe* P){ pq_pipe_finish(B, P); }

static inline void pq_tex_hvx3(const pq_bufs* B, pq_pipe* P, const int* seg, int count, unsigned char* pdest, int base_off, int cw,
                               const unsigned char* pv, int vbytes){
    const int nseg = (count + 7) >> 3;
    const int cw2 = (int)(((unsigned)cw & 0xFFFFu) * 0x10001u);   /* vmpyi(Vw, Rt.h) takes Rt.h[lane & 1] */
    const HVX_Vector lim = Q6_V_vsplat_R(vbytes - 1);
    const int me = P->cur ^ 1;                                    /* the slot the pending chunk does not use */
    P->cur = me;
    pq_sp* S = &P->sp[me];
    S->seg = seg; S->count = count; S->pdest = pdest; S->base_off = base_off; S->cw = cw; S->pv = pv; S->vbytes = vbytes;
    S->decided = 0; S->isbad = 0;
    HVX_Vector bad = Q6_V_vzero();
    HVX_Vector* gsv = (HVX_Vector*)B->gs;
    for(int g = 0; g < nseg; g += 32){
        const int ng = nseg - g < 32 ? nseg - g : 32;
        HVX_Vector Sv, Tv, SS, TS;
        pq_load_seg(B, seg + 4 * g, ng, &Sv, &Tv, &SS, &TS);
        if(ng < 32){                                              /* lanes q >= ng: index 0 */
            const HVX_VectorPred qv = Q6_Q_vsetq_R(4 * ng);
            Sv = Q6_V_vmux_QVV(qv, Sv, Q6_V_vzero()); Tv = Q6_V_vmux_QVV(qv, Tv, Q6_V_vzero());
            SS = Q6_V_vmux_QVV(qv, SS, Q6_V_vzero()); TS = Q6_V_vmux_QVV(qv, TS, Q6_V_vzero());
        }
        const int last = (g + 32 >= nseg);
        const int nl = count - 8 * (nseg - 1);                    /* real pixels of the last segment (1..8) */
        const HVX_VectorPred qlast = Q6_Q_vsetq_R(4 * (ng - 1));  /* lanes before the last segment (ng-1 = 0: none) */
        HVX_Vector I[8];
        for(int j = 0; j < 8; j++){
            HVX_Vector Ij = Q6_Vw_vadd_VwVw(Q6_Vw_vasr_VwR(Sv, 16), Q6_Vw_vmpyi_VwRh(Q6_Vw_vasr_VwR(Tv, 16), cw2));
            if(last && j >= nl) Ij = Q6_V_vmux_QVV(qlast, Ij, Q6_V_vzero());   /* virtual pixels: index 0 */
            I[j] = Ij;
            bad = Q6_V_vor_VV(bad, Q6_V_vor_VV(Ij, Q6_Vw_vsub_VwVw(lim, Ij)));
            Sv = Q6_Vw_vadd_VwVw(Sv, SS); Tv = Q6_Vw_vadd_VwVw(Tv, TS);
        }
        S->bad = bad;                                             /* complete once the last group is done */
        const HVX_VectorPair P01 = Q6_W_vshuff_VVR(I[1], I[0], -4), P23 = Q6_W_vshuff_VVR(I[3], I[2], -4);
        const HVX_VectorPair P45 = Q6_W_vshuff_VVR(I[5], I[4], -4), P67 = Q6_W_vshuff_VVR(I[7], I[6], -4);
        const HVX_VectorPair Xa = Q6_W_vshuff_VVR(Q6_V_lo_W(P23), Q6_V_lo_W(P01), -8);
        const HVX_VectorPair Xb = Q6_W_vshuff_VVR(Q6_V_hi_W(P23), Q6_V_hi_W(P01), -8);
        const HVX_VectorPair Ya = Q6_W_vshuff_VVR(Q6_V_lo_W(P67), Q6_V_lo_W(P45), -8);
        const HVX_VectorPair Yb = Q6_W_vshuff_VVR(Q6_V_hi_W(P67), Q6_V_hi_W(P45), -8);
        HVX_VectorPair Q[4];
        Q[0] = Q6_W_vshuff_VVR(Q6_V_lo_W(Ya), Q6_V_lo_W(Xa), -16);
        Q[1] = Q6_W_vshuff_VVR(Q6_V_hi_W(Ya), Q6_V_hi_W(Xa), -16);
        Q[2] = Q6_W_vshuff_VVR(Q6_V_lo_W(Yb), Q6_V_lo_W(Xb), -16);
        Q[3] = Q6_W_vshuff_VVR(Q6_V_hi_W(Yb), Q6_V_hi_W(Xb), -16);
        const int gpix = 8 * g;
        for(int c = 0; c < 4; c += 2){
            const int p0 = gpix + 64 * c;
            if(p0 >= count) break;
            const int slot = P->chunk & 1; P->chunk++;
            HVX_Vector* gt = gsv + 2 * slot;
            const int two = p0 + 64 < count;
#ifndef PQ_NOGATHER_DIAG
            Q6_vgather_ARMVh(gt, (unsigned)(uintptr_t)pv, vbytes - 1, Q6_Vh_vpacke_VwVw(Q6_V_hi_W(Q[c]), Q6_V_lo_W(Q[c])));
            if(two) Q6_vgather_ARMVh(gt + 1, (unsigned)(uintptr_t)pv, vbytes - 1, Q6_Vh_vpacke_VwVw(Q6_V_hi_W(Q[c + 1]), Q6_V_lo_W(Q[c + 1])));
#else
            gt[0] = Q6_Vh_vpacke_VwVw(Q6_V_hi_W(Q[c]), Q6_V_lo_W(Q[c])); if(two) gt[1] = Q6_Vh_vpacke_VwVw(Q6_V_hi_W(Q[c + 1]), Q6_V_lo_W(Q[c + 1]));
#endif
            /* a chunk of THIS span may only be stored once the whole span's indices are checked: if this span has
             * more groups to go, finishing our own earlier chunk would read an incomplete `bad` - then decide now
             * on the full span by finishing only chunks of OTHER spans; own chunks wait (see below) */
            if(P->pend_active && P->pend_sp == me && g + 32 < nseg && !S->decided){
                /* own chunk pending and more groups left: compute the rest of `bad` first (rare: > 256 px) */
                HVX_Vector bad2 = bad;
                for(int g2 = g + 32; g2 < nseg; g2 += 32){
                    const int ng2 = nseg - g2 < 32 ? nseg - g2 : 32;
                    HVX_Vector S2, T2, SS2, TS2;
                    pq_load_seg(B, seg + 4 * g2, ng2, &S2, &T2, &SS2, &TS2);
                    if(ng2 < 32){
                        const HVX_VectorPred qv = Q6_Q_vsetq_R(4 * ng2);
                        S2 = Q6_V_vmux_QVV(qv, S2, Q6_V_vzero()); T2 = Q6_V_vmux_QVV(qv, T2, Q6_V_vzero());
                        SS2 = Q6_V_vmux_QVV(qv, SS2, Q6_V_vzero()); TS2 = Q6_V_vmux_QVV(qv, TS2, Q6_V_vzero());
                    }
                    const int last2 = (g2 + 32 >= nseg), nl2 = count - 8 * (nseg - 1);
                    const HVX_VectorPred ql2 = Q6_Q_vsetq_R(4 * (ng2 - 1));
                    for(int j = 0; j < 8; j++){
                        HVX_Vector I2 = Q6_Vw_vadd_VwVw(Q6_Vw_vasr_VwR(S2, 16), Q6_Vw_vmpyi_VwRh(Q6_Vw_vasr_VwR(T2, 16), cw2));
                        if(last2 && j >= nl2) I2 = Q6_V_vmux_QVV(ql2, I2, Q6_V_vzero());
                        bad2 = Q6_V_vor_VV(bad2, Q6_V_vor_VV(I2, Q6_Vw_vsub_VwVw(lim, I2)));
                        S2 = Q6_Vw_vadd_VwVw(S2, SS2); T2 = Q6_Vw_vadd_VwVw(T2, TS2);
                    }
                }
                S->bad = bad2; bad = bad2;
                if(S->decided == 0){ S->decided = 1; S->isbad = pq_any(Q6_V_vand_VV(bad2, Q6_V_vsplat_R((int)0x80000000)));
                    if(S->isbad){ P->fallbacks++; pq_tex_scalar(B, seg, count, pdest, base_off, cw, pv, vbytes, &P->oob); } }
            }
            pq_pipe_finish(B, P);
            P->pend_active = 1; P->pend_sp = me; P->pend_dst = pdest + p0; P->pend_n = count - p0 >= 128 ? 128 : count - p0;
            P->pend_slot = slot; P->pend_two = two;
        }
    }
}
#endif
