/* pq_tex2.h - HVX texture of one span, version 2 (2026-10-26, phase 3). Included by pq_kern.h in place of version 1.
 * Pass A: the indices in segment-lane space (lane = segment, one vector per pixel j of the segments), out-of-block test
 *         of every VALID pixel (8q + j < count), ONE reduction per span -> fallback before anything is written.
 * Pass B: the same indices again (cheap), transposed to pixel order, gathered with a one-chunk lag: the gathers of
 *         chunk c are issued before chunk c-1's result is read, so a gather's latency overlaps the next chunk's work.
 *         Gather targets: 4 x 128 B per thread (two chunks in flight, two halves each).
 * Same arithmetic as version 1 (proven in hexagon-sim and on the NPU). */
#ifndef PQ_TEX2_H
#define PQ_TEX2_H

static inline void pq_load_seg(const pq_bufs* B, const int* src, int ng, HVX_Vector* S, HVX_Vector* T, HVX_Vector* SS, HVX_Vector* TS){
    HVX_Vector X0, X1, X2, X3;
    if(src + 128 <= B->cmds_end){
        X0 = *(const pq_uvec*)(src); X1 = *(const pq_uvec*)(src + 32); X2 = *(const pq_uvec*)(src + 64); X3 = *(const pq_uvec*)(src + 96);
    } else {
        int __attribute__((aligned(128))) tmp[128];
        for(int i = 0; i < 4 * ng; i++) tmp[i] = src[i];
        for(int i = 4 * ng; i < 128; i++) tmp[i] = 0;
        X0 = *(const HVX_Vector*)(tmp); X1 = *(const HVX_Vector*)(tmp + 32); X2 = *(const HVX_Vector*)(tmp + 64); X3 = *(const HVX_Vector*)(tmp + 96);
    }
    const HVX_VectorPair d01 = Q6_W_vdeal_VVR(X1, X0, -4), d23 = Q6_W_vdeal_VVR(X3, X2, -4);
    const HVX_VectorPair dS = Q6_W_vdeal_VVR(Q6_V_lo_W(d23), Q6_V_lo_W(d01), -4);
    const HVX_VectorPair dT = Q6_W_vdeal_VVR(Q6_V_hi_W(d23), Q6_V_hi_W(d01), -4);
    *S = Q6_V_lo_W(dS); *SS = Q6_V_hi_W(dS); *T = Q6_V_lo_W(dT); *TS = Q6_V_hi_W(dT);
}

static inline int pq_tex_hvx(const pq_bufs* B, const int* seg, int count, unsigned char* pdest, int cw,
                             const unsigned char* pv, int vbytes){
    const int nseg = (count + 7) >> 3;
    const int cw2 = (int)(((unsigned)cw & 0xFFFFu) * 0x10001u);   /* vmpyi(Vw, Rt.h) takes Rt.h[lane & 1] */
    const HVX_Vector lim = Q6_V_vsplat_R(vbytes - 1);
    const HVX_Vector ones = Q6_V_vsplat_R(-1);
    /* ---- pass A: every valid pixel inside the block? */
    HVX_Vector bad = Q6_V_vzero();
    for(int g = 0; g < nseg; g += 32){
        const int ng = nseg - g < 32 ? nseg - g : 32;
        HVX_Vector S, T, SS, TS;
        pq_load_seg(B, seg + 4 * g, ng, &S, &T, &SS, &TS);
        const int left = count - 8 * g;                          /* pixels from this group on */
        for(int j = 0; j < 8; j++){
            int nv = (left - j + 7) >> 3; if(nv > 32) nv = 32;  /* segments q with 8q + j < left */
            if(nv > 0){
                const HVX_Vector I = Q6_Vw_vadd_VwVw(Q6_Vw_vasr_VwR(S, 16), Q6_Vw_vmpyi_VwRh(Q6_Vw_vasr_VwR(T, 16), cw2));
                bad = Q6_V_vmux_QVV(Q6_Q_and_QQ(Q6_Q_vcmp_gt_VuwVuw(I, lim), Q6_Q_vsetq2_R(4 * nv)), ones, bad);
            }
            S = Q6_Vw_vadd_VwVw(S, SS); T = Q6_Vw_vadd_VwVw(T, TS);
        }
    }
    if(pq_any(bad)) return 1;
    /* ---- pass B: gathers with a one-chunk lag */
    HVX_Vector* gsv = (HVX_Vector*)B->gs;                        /* 4 vectors: [slot][half] */
#ifndef PQ_GATHER_ODD_DIRECT
    const HVX_Vector one16 = Q6_Vh_vsplat_R(1), even16 = Q6_Vh_vsplat_R(0xFFFE);
    HVX_VectorPred podd0 = Q6_Q_vsetq_R(0), podd1 = Q6_Q_vsetq_R(0);
#endif
    int pend_p0 = -1, pend_slot = 0, chunk = 0;
    for(int g = 0; g < nseg; g += 32){
        const int ng = nseg - g < 32 ? nseg - g : 32;
        HVX_Vector S, T, SS, TS;
        pq_load_seg(B, seg + 4 * g, ng, &S, &T, &SS, &TS);
        HVX_Vector I[8];
        for(int j = 0; j < 8; j++){
            I[j] = Q6_Vw_vadd_VwVw(Q6_Vw_vasr_VwR(S, 16), Q6_Vw_vmpyi_VwRh(Q6_Vw_vasr_VwR(T, 16), cw2));
            S = Q6_Vw_vadd_VwVw(S, SS); T = Q6_Vw_vadd_VwVw(T, TS);
        }
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
            const int slot = chunk & 1; chunk++;
            HVX_Vector* gt = gsv + 2 * slot;
            const HVX_Vector off0 = Q6_Vh_vpacke_VwVw(Q6_V_hi_W(Q[c]), Q6_V_lo_W(Q[c]));
            const HVX_Vector off1 = Q6_Vh_vpacke_VwVw(Q6_V_hi_W(Q[c + 1]), Q6_V_lo_W(Q[c + 1]));
            const int two = p0 + 64 < count;
#ifdef PQ_GATHER_ODD_DIRECT
            Q6_vgather_ARMVh(gt, (unsigned)(uintptr_t)pv, vbytes - 1, off0);
            if(two) Q6_vgather_ARMVh(gt + 1, (unsigned)(uintptr_t)pv, vbytes - 1, off1);
#else
            const HVX_VectorPred o0 = Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(off0, one16), one16);
            const HVX_VectorPred o1 = Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(off1, one16), one16);
            Q6_vgather_ARMVh(gt, (unsigned)(uintptr_t)pv, vbytes - 1, Q6_V_vand_VV(off0, even16));
            if(two) Q6_vgather_ARMVh(gt + 1, (unsigned)(uintptr_t)pv, vbytes - 1, Q6_V_vand_VV(off1, even16));
#endif
            if(pend_p0 >= 0){                                    /* finish the previous chunk */
                HVX_Vector* pt = gsv + 2 * pend_slot;
                HVX_Vector G0 = pt[0], G1 = (pend_p0 + 64 < count) ? pt[1] : Q6_V_vzero();
#ifndef PQ_GATHER_ODD_DIRECT
                G0 = Q6_V_vmux_QVV(podd0, Q6_Vuh_vlsr_VuhR(G0, 8), G0);
                G1 = Q6_V_vmux_QVV(podd1, Q6_Vuh_vlsr_VuhR(G1, 8), G1);
#endif
                const int n = count - pend_p0 >= 128 ? 128 : count - pend_p0;
                pq_st(pdest + pend_p0, Q6_Vb_vpacke_VhVh(G1, G0), n);
            }
#ifndef PQ_GATHER_ODD_DIRECT
            podd0 = o0; podd1 = o1;
#endif
            pend_p0 = p0; pend_slot = slot;
        }
    }
    if(pend_p0 >= 0){
        HVX_Vector* pt = gsv + 2 * pend_slot;
        HVX_Vector G0 = pt[0], G1 = (pend_p0 + 64 < count) ? pt[1] : Q6_V_vzero();
#ifndef PQ_GATHER_ODD_DIRECT
        G0 = Q6_V_vmux_QVV(podd0, Q6_Vuh_vlsr_VuhR(G0, 8), G0);
        G1 = Q6_V_vmux_QVV(podd1, Q6_Vuh_vlsr_VuhR(G1, 8), G1);
#endif
        const int n = count - pend_p0 >= 128 ? 128 : count - pend_p0;
        pq_st(pdest + pend_p0, Q6_Vb_vpacke_VhVh(G1, G0), n);
    }
    return 0;
}
#endif
