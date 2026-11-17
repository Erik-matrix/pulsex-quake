/* pq_tex4.h - HVX texture, version 4 (2026-10-26, phase 3).
 * hexagon-sim (pq_simbench.c) showed versions 2/3 at ~560 cycles per span even for 16 pixels: the code per group of
 * 32 segments was ~500 packets (the out-of-block test per pixel column with predicates, the last-segment special
 * cases, all 12 shuffles even for a short span), and Quake's spans are short (avg 64 px at 640, 130 at 1280).
 * Version 4:
 *   - out-of-block test PER 128-PIXEL CHUNK, in pixel order after the transpose: valid lanes (pixel < count) only,
 *     acc |= W | (lim - W), bad iff a sign bit is set (|W| < 2^30, so lim - W does not overflow).
 *     Correctness argument: a chunk is stored only if ALL its valid pixels are inside the block, and then its bytes
 *     equal the scalar loop's (same VTCM/shared bytes). A bad chunk is never stored; the scalar loop then redraws the
 *     WHOLE span once. Pixels the scalar loop skips (outside the cache) lie only in bad chunks, so nothing HVX wrote
 *     remains where the scalar loop would have left the old byte. Good chunks of a bad span store the same bytes the
 *     scalar loop writes, in any order.
 *   - only the shuffles the span needs (<= 8 segments: 7, <= 16: 8, else 12);
 *   - the gather pipeline across spans (version 3), the chunk's test read one chunk later (latency hidden).
 * Same index arithmetic as versions 1-3. */
#ifndef PQ_TEX4_H
#define PQ_TEX4_H

typedef struct {
    int active, n, slot, two; unsigned char* dst; HVX_Vector bad;
    const int* seg; int count; unsigned char* pdest; int base_off, cw; const unsigned char* pv; int vbytes;
} pq_pend4;
typedef struct { pq_pend4 p; int chunk; const int* redrawn; int fallbacks, oob; } pq_pipe4;

static inline void pq_pipe4_init(pq_pipe4* P){ P->p.active = 0; P->chunk = 0; P->redrawn = 0; P->fallbacks = 0; P->oob = 0; }
static inline void pq_pipe4_finish(const pq_bufs* B, pq_pipe4* P){
    pq_pend4* q = &P->p;
    if(!q->active) return;
    q->active = 0;
#ifndef PQ_DIAG_NOANY
    if(pq_any(Q6_V_vand_VV(q->bad, Q6_V_vsplat_R((int)0x80000000)))){
        if(P->redrawn != q->seg){ P->redrawn = q->seg; P->fallbacks++;
            pq_tex_scalar(B, q->seg, q->count, q->pdest, q->base_off, q->cw, q->pv, q->vbytes, &P->oob); }
        return;
    }
#endif
    const HVX_Vector* pt = (const HVX_Vector*)B->gs + 2 * q->slot;
    const HVX_Vector G0 = pt[0], G1 = q->two ? pt[1] : Q6_V_vzero();
#ifndef PQ_DIAG_NOST
    pq_st(q->dst, Q6_Vb_vpacke_VhVh(G1, G0), q->n);
#else
    *(volatile HVX_Vector*)B->gs = Q6_Vb_vpacke_VhVh(G1, G0);
#endif
}
static inline void pq_pipe4_end(const pq_bufs* B, pq_pipe4* P){ pq_pipe4_finish(B, P); }

static inline HVX_Vector pq_badw(HVX_Vector W, HVX_Vector lim, int nv){
    /* nv = valid lanes (may be <= 0 or > 32) */
    if(nv <= 0) return Q6_V_vzero();
    const HVX_Vector t = Q6_V_vor_VV(W, Q6_Vw_vsub_VwVw(lim, W));
    if(nv >= 32) return t;
    return Q6_V_vmux_QVV(Q6_Q_vsetq_R(4 * nv), t, Q6_V_vzero());
}

static inline void pq_tex_hvx4(const pq_bufs* B, pq_pipe4* P, const int* seg, int count, unsigned char* pdest, int base_off, int cw,
                               const unsigned char* pv, int vbytes){
    const int nseg = (count + 7) >> 3;
    const int cw2 = (int)(((unsigned)cw & 0xFFFFu) * 0x10001u);   /* vmpyi(Vw, Rt.h) takes Rt.h[lane & 1] */
    const HVX_Vector lim = Q6_V_vsplat_R(vbytes - 1);
    HVX_Vector* gsv = (HVX_Vector*)B->gs;
    for(int g = 0; g < nseg; g += 32){
        const int ng = nseg - g < 32 ? nseg - g : 32;
        HVX_Vector Sv, Tv, SS, TS;
        pq_load_seg(B, seg + 4 * g, ng, &Sv, &Tv, &SS, &TS);
        HVX_Vector I[8];
        for(int j = 0; j < 8; j++){
            I[j] = Q6_Vw_vadd_VwVw(Q6_Vw_vasr_VwR(Sv, 16), Q6_Vw_vmpyi_VwRh(Q6_Vw_vasr_VwR(Tv, 16), cw2));
            Sv = Q6_Vw_vadd_VwVw(Sv, SS); Tv = Q6_Vw_vadd_VwVw(Tv, TS);
        }
        const HVX_VectorPair P01 = Q6_W_vshuff_VVR(I[1], I[0], -4), P23 = Q6_W_vshuff_VVR(I[3], I[2], -4);
        const HVX_VectorPair P45 = Q6_W_vshuff_VVR(I[5], I[4], -4), P67 = Q6_W_vshuff_VVR(I[7], I[6], -4);
        HVX_VectorPair Q[4];
        {
            const HVX_VectorPair Xa = Q6_W_vshuff_VVR(Q6_V_lo_W(P23), Q6_V_lo_W(P01), -8);
            const HVX_VectorPair Ya = Q6_W_vshuff_VVR(Q6_V_lo_W(P67), Q6_V_lo_W(P45), -8);
            Q[0] = Q6_W_vshuff_VVR(Q6_V_lo_W(Ya), Q6_V_lo_W(Xa), -16);
            if(ng > 8) Q[1] = Q6_W_vshuff_VVR(Q6_V_hi_W(Ya), Q6_V_hi_W(Xa), -16);
            if(ng > 16){
                const HVX_VectorPair Xb = Q6_W_vshuff_VVR(Q6_V_hi_W(P23), Q6_V_hi_W(P01), -8);
                const HVX_VectorPair Yb = Q6_W_vshuff_VVR(Q6_V_hi_W(P67), Q6_V_hi_W(P45), -8);
                Q[2] = Q6_W_vshuff_VVR(Q6_V_lo_W(Yb), Q6_V_lo_W(Xb), -16);
                if(ng > 24) Q[3] = Q6_W_vshuff_VVR(Q6_V_hi_W(Yb), Q6_V_hi_W(Xb), -16);
            }
        }
        const int gpix = 8 * g;
        for(int c = 0; c < 4; c += 2){
            const int p0 = gpix + 64 * c;
            if(p0 >= count) break;
            const int slot = P->chunk & 1; P->chunk++;
            HVX_Vector* gt = gsv + 2 * slot;
            const int two = p0 + 64 < count;
            const int nv = count - p0;                          /* valid pixels from p0 on */
            HVX_Vector bad = Q6_V_vor_VV(pq_badw(Q6_V_lo_W(Q[c]), lim, nv), pq_badw(Q6_V_hi_W(Q[c]), lim, nv - 32));
            Q6_vgather_ARMVh(gt, (unsigned)(uintptr_t)pv, vbytes - 1, Q6_Vh_vpacke_VwVw(Q6_V_hi_W(Q[c]), Q6_V_lo_W(Q[c])));
            if(two){
                bad = Q6_V_vor_VV(bad, Q6_V_vor_VV(pq_badw(Q6_V_lo_W(Q[c + 1]), lim, nv - 64), pq_badw(Q6_V_hi_W(Q[c + 1]), lim, nv - 96)));
                Q6_vgather_ARMVh(gt + 1, (unsigned)(uintptr_t)pv, vbytes - 1, Q6_Vh_vpacke_VwVw(Q6_V_hi_W(Q[c + 1]), Q6_V_lo_W(Q[c + 1])));
            }
            pq_pipe4_finish(B, P);                               /* the previous chunk (often of the previous span) */
            pq_pend4* q = &P->p;
            q->active = 1; q->n = nv >= 128 ? 128 : nv; q->slot = slot; q->two = two; q->dst = pdest + p0; q->bad = bad;
            q->seg = seg; q->count = count; q->pdest = pdest; q->base_off = base_off; q->cw = cw; q->pv = pv; q->vbytes = vbytes;
        }
    }
}
#endif
