/* pq_tex5.h - HVX texture, version 5 (2026-10-26, phase 3): no vector->scalar transfer on the hot path.
 * hexagon-sim (pq_simbench.c, 130-px spans): version 4 = 613 cycles/span, of which ~400 were the per-chunk
 * out-of-block reduction (vextract stalls until the vector pipeline - gathers included - has drained).
 * Version 5: every chunk stores ONLY its in-block valid pixels (byte predicate from the index test), and ORs its
 * out-of-block mask into one accumulator per thread. After the thread's last span ONE reduction: if any pixel was out
 * of its block (never in the demos), the scalar loop redraws ALL of the thread's spans. That is exact: HVX wrote only
 * in-block pixels, with the bytes the scalar loop writes there; out-of-block pixels were left untouched by HVX, so
 * the scalar redraw leaves exactly the scalar result (including the bytes it skips outside the cache).
 * Index arithmetic, transposition, gathers (direct odd offsets) as versions 1-4. */
#ifndef PQ_TEX5_H
#define PQ_TEX5_H

typedef struct { int active, n, slot, two; unsigned char* dst; HVX_Vector badb; } pq_pend5;
typedef struct { pq_pend5 p; int chunk; HVX_Vector acc; } pq_pipe5;

static inline void pq_pipe5_init(pq_pipe5* P){ P->p.active = 0; P->chunk = 0; P->acc = Q6_V_vzero(); }

/* store the first n bytes (1..128) of v at dst, skipping bytes whose badb byte is non-zero */
static inline void pq_st_m(void* dst, HVX_Vector v, HVX_Vector badb, int n){
    const uintptr_t a = (uintptr_t)dst; const int off = (int)(a & 127u);
    HVX_Vector* b = (HVX_Vector*)(a - (uintptr_t)off);
    const int rot = (128 - off) & 127;
    const HVX_Vector r = Q6_V_vror_VR(v, rot);
    const HVX_VectorPred ok = Q6_Q_vcmp_eq_VbVb(Q6_V_vror_VR(badb, rot), Q6_V_vzero());
    const int end = off + n;
    const HVX_VectorPred qe = Q6_Q_vsetq2_R(end >= 128 ? 128 : end);
    Q6_vmem_QRIV(Q6_Q_and_QQ(Q6_Q_and_QQn(qe, Q6_Q_vsetq_R(off)), ok), b, r);
    if(end > 128) Q6_vmem_QRIV(Q6_Q_and_QQ(Q6_Q_vsetq_R(end - 128), ok), b + 1, r);
}
static inline void pq_pipe5_finish(const pq_bufs* B, pq_pipe5* P){
    pq_pend5* q = &P->p;
    if(!q->active) return;
    q->active = 0;
    const HVX_Vector* pt = (const HVX_Vector*)B->gs + 2 * q->slot;
    const HVX_Vector G0 = pt[0], G1 = q->two ? pt[1] : Q6_V_vzero();
    pq_st_m(q->dst, Q6_Vb_vpacke_VhVh(G1, G0), q->badb, q->n);
}
/* returns 1 if any pixel of the thread's spans was outside its block (caller redraws them all, scalar) */
static inline int pq_pipe5_end(const pq_bufs* B, pq_pipe5* P){ pq_pipe5_finish(B, P); return pq_any(P->acc); }

/* -1 per word whose index is outside [0, lim] or whose lane is not a valid pixel (lane >= nv): those are not stored */
static inline HVX_Vector pq_badw5(HVX_Vector W, HVX_Vector lim, int nv){
    const HVX_Vector t = Q6_Vw_vasr_VwR(Q6_V_vor_VV(W, Q6_Vw_vsub_VwVw(lim, W)), 31);   /* -1 = out of block */
    if(nv >= 32) return t;
    if(nv <= 0) return Q6_V_vsplat_R(-1);
    return Q6_V_vmux_QVV(Q6_Q_vsetq_R(4 * nv), t, Q6_V_vsplat_R(-1));
}

static inline void pq_tex_hvx5(const pq_bufs* B, pq_pipe5* P, const int* seg, int count, unsigned char* pdest, int cw,
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
            Q[1] = Q6_W_vshuff_VVR(Q6_V_hi_W(Ya), Q6_V_hi_W(Xa), -16);
            if(ng > 16){
                const HVX_VectorPair Xb = Q6_W_vshuff_VVR(Q6_V_hi_W(P23), Q6_V_hi_W(P01), -8);
                const HVX_VectorPair Yb = Q6_W_vshuff_VVR(Q6_V_hi_W(P67), Q6_V_hi_W(P45), -8);
                Q[2] = Q6_W_vshuff_VVR(Q6_V_lo_W(Yb), Q6_V_lo_W(Xb), -16);
                Q[3] = Q6_W_vshuff_VVR(Q6_V_hi_W(Yb), Q6_V_hi_W(Xb), -16);
            } else { Q[2] = Q[0]; Q[3] = Q[0]; }
        }
        const int gpix = 8 * g;
        for(int c = 0; c < 4; c += 2){
            const int p0 = gpix + 64 * c;
            if(p0 >= count) break;
            const int slot = P->chunk & 1; P->chunk++;
            HVX_Vector* gt = gsv + 2 * slot;
            const int two = p0 + 64 < count;
            const int nv = count - p0;
            Q6_vgather_ARMVh(gt, (unsigned)(uintptr_t)pv, vbytes - 1, Q6_Vh_vpacke_VwVw(Q6_V_hi_W(Q[c]), Q6_V_lo_W(Q[c])));
            if(two) Q6_vgather_ARMVh(gt + 1, (unsigned)(uintptr_t)pv, vbytes - 1, Q6_Vh_vpacke_VwVw(Q6_V_hi_W(Q[c + 1]), Q6_V_lo_W(Q[c + 1])));
            /* per-pixel "do not store" bytes: words -> halfwords -> bytes (vpacke keeps the low part: 0 or -1) */
            const HVX_Vector b0 = Q6_Vh_vpacke_VwVw(pq_badw5(Q6_V_hi_W(Q[c]), lim, nv - 32), pq_badw5(Q6_V_lo_W(Q[c]), lim, nv));
            const HVX_Vector b1 = two ? Q6_Vh_vpacke_VwVw(pq_badw5(Q6_V_hi_W(Q[c + 1]), lim, nv - 96), pq_badw5(Q6_V_lo_W(Q[c + 1]), lim, nv - 64))
                                      : Q6_V_vsplat_R(-1);
            const HVX_Vector badb = Q6_Vb_vpacke_VhVh(b1, b0);
            /* only real pixels count as out of block: the invalid lanes are -1 too, so mask them for the accumulator */
            const HVX_Vector realb = (nv >= 128) ? badb : Q6_V_vmux_QVV(Q6_Q_vsetq_R(nv), badb, Q6_V_vzero());
            P->acc = Q6_V_vor_VV(P->acc, realb);
            pq_pipe5_finish(B, P);
            pq_pend5* q = &P->p;
            q->active = 1; q->n = nv >= 128 ? 128 : nv; q->slot = slot; q->two = two; q->dst = pdest + p0; q->badb = badb;
        }
    }
}
#endif
