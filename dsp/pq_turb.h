/* pq_turb.h - PulseX Quake phase 6 (2026-10-26): the water (Turbulent8 + D_DrawZSpans) on the DSP.
 * pq_turb_span: scalar, id's D_DrawTurbulent8Span per 16-pixel segment. pq_turb_span_hvx3: HVX, 128 pixels per
 * round, the turbulence table by vlut32 in byte lanes, the texel by vgather .h, pending gathers. Both validate the
 * op-5 record and its op-6 texture record first. Used by pxquake_imp.c and pq_turbsim.c (hexagon-sim). */
#ifndef PQ_TURB_H
#define PQ_TURB_H
#include <string.h>

/* phase 6: one water span (op 5), exactly id's D_DrawTurbulent8Span per 16-pixel segment, then D_DrawZSpans.
 * Texture and turbulence table come from the stream's op-6 record. Everything is validated first. */
static void pq_turb_span(const int* cmds, int tab_off, int pos, unsigned char* fb, int fbLen, short* zb, int zbLen, int* npix, int* bad){
    if(pos < 8 || pos + 7 > tab_off || cmds[pos] != 5){ (*bad)++; return; }
    const int fb_off = cmds[pos + 1], z_off = cmds[pos + 2], count = cmds[pos + 3], tp = cmds[pos + 6];
    unsigned izi = (unsigned)cmds[pos + 4]; const unsigned izistep = (unsigned)cmds[pos + 5];
    const int nseg = (count + 15) >> 4;
    if(count <= 0 || pos + 7 + 4 * nseg > tab_off || tp < 8 || tp + 2 + 128 + 1024 > tab_off || cmds[tp] != 6
       || fb_off < 0 || fb_off + count > fbLen || z_off < 0 || z_off + count > zbLen){ (*bad)++; return; }
    const int* turb = cmds + tp + 2; const unsigned char* tex = (const unsigned char*)(cmds + tp + 130);
    const int* seg = cmds + pos + 7;
    unsigned char* pdest = fb + fb_off;
    int left = count;
    for(int k = 0; k < nseg; k++){
        int n = left >= 16 ? 16 : left; left -= n;
        int s = seg[4 * k], t = seg[4 * k + 1]; const int ss = seg[4 * k + 2], ts = seg[4 * k + 3];
        do {
            const int sturb = ((s + turb[(t >> 16) & 127]) >> 16) & 63;
            const int tturb = ((t + turb[(s >> 16) & 127]) >> 16) & 63;
            *pdest++ = tex[(tturb << 6) + sturb];
            s += ss; t += ts;
        } while(--n > 0);
    }
    *npix += count;
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
/* water HVX version 2 (vlut32 byte lanes); same contract as version 1 */
static inline HVX_Vector pq_lut128(HVX_Vector idx, HVX_Vector tab){
#ifdef PQ_TURB_DIAG_NOLUT
    return Q6_V_vxor_VV(idx, tab);
#endif
    HVX_Vector r = Q6_Vb_vlut32_VbVbI(idx, tab, 0);
    r = Q6_Vb_vlut32or_VbVbVbI(r, idx, tab, 1);
    r = Q6_Vb_vlut32or_VbVbVbI(r, idx, tab, 2);
    r = Q6_Vb_vlut32or_VbVbVbI(r, idx, tab, 3);
    return r;
}
/* ((x + turb[i]) >> 16) & 63 per byte lane, from x's bytes 0, 1, 2 and turb[i]'s bytes 0, 1, 2 */
static inline HVX_Vector pq_turb_add(HVX_Vector x0, HVX_Vector x1, HVX_Vector x2, HVX_Vector u0, HVX_Vector u1, HVX_Vector u2){
    const HVX_Vector one = Q6_Vb_vsplat_R(1), zero = Q6_V_vzero();
    const HVX_Vector ls = Q6_Vb_vadd_VbVb(x0, u0);
    const HVX_VectorPred c0 = Q6_Q_vcmp_gt_VubVub(x0, ls);                    /* the low byte sum wrapped */
    const HVX_Vector hs = Q6_Vb_vadd_VbVb(x1, u1);
    const HVX_VectorPred c1 = Q6_Q_vcmp_gt_VubVub(x1, hs);                    /* the high byte sum wrapped */
    const HVX_VectorPred c2 = Q6_Q_and_QQ(c0, Q6_Q_vcmp_eq_VbVb(hs, Q6_Vb_vsplat_R(-1)));   /* ... or wraps with c0 */
    const HVX_Vector c = Q6_V_vmux_QVV(Q6_Q_or_QQ(c1, c2), one, zero);
    return Q6_V_vand_VV(Q6_Vb_vadd_VbVb(Q6_Vb_vadd_VbVb(x2, u2), c), Q6_Vb_vsplat_R(63));
}
/* water HVX version 3: version 2 + pending texel gathers (2 slots) + dcfetch of the next span. Thread area tv:
 * [512, 4608) texture, [4608, 4736) pad, [4736, 5248) 2 slots x 2 gather targets, [5248, 5632) the 3 vlut tables */
typedef struct { unsigned char* tv; int staged; int active, n, slot, two, chunk; unsigned char* dst; } pq_tctx;
static inline void pq_tfinish(pq_tctx* X){
    if(!X->active) return;
    X->active = 0;
    const HVX_Vector* g = (const HVX_Vector*)(X->tv + 4736) + 2 * X->slot;
    const HVX_Vector px = Q6_Vb_vpacke_VhVh(X->two ? g[1] : Q6_V_vzero(), g[0]);
#ifndef PQ_TURB_DIAG_NOSTORE
    pq_st(X->dst, px, X->n);
#else
    *(volatile HVX_Vector*)(X->tv + 4736 + 384) = px;
#endif
}
static inline void pq_turb_pf(const int* cmds, int tab_off, int pos, unsigned char* fb, int fbLen, short* zb, int zbLen){
    if(pos < 8 || pos + 7 > tab_off || cmds[pos] != 5) return;
    const int fb_off = cmds[pos + 1], z_off = cmds[pos + 2], count = cmds[pos + 3];
    if(count <= 0 || fb_off < 0 || fb_off + count > fbLen || z_off < 0 || z_off + count > zbLen) return;
    Q6_dcfetch_A(fb + fb_off); Q6_dcfetch_A(fb + fb_off + count - 1);
    Q6_dcfetch_A(zb + z_off); Q6_dcfetch_A((unsigned char*)(zb + z_off) + count); Q6_dcfetch_A(zb + z_off + count - 1);
}
static int pq_turb_span_hvx3(const int* cmds, int tab_off, int pos, unsigned char* fb, int fbLen, short* zb, int zbLen,
                             pq_tctx* X, int* npix){
    if(pos < 8 || pos + 7 > tab_off || cmds[pos] != 5) return 0;
    const int fb_off = cmds[pos + 1], z_off = cmds[pos + 2], count = cmds[pos + 3], tp = cmds[pos + 6];
    const int nseg = (count + 15) >> 4;
    if(count <= 0 || pos + 7 + 4 * nseg > tab_off || tp < 8 || tp + 2 + 128 + 1024 > tab_off || cmds[tp] != 6
       || fb_off < 0 || fb_off + count > fbLen || z_off < 0 || z_off + count > zbLen) return 0;   /* the scalar loop counts it */
    unsigned char* tv = X->tv;
    unsigned char* tabs = tv + 5248;                             /* TH (byte 2), T0 (byte 0), T1 (byte 1) of turb[] */
    if(X->staged != tp){
        pq_tfinish(X);                                           /* its gathers read the texture about to be replaced */
        memcpy(tv + 512, cmds + tp + 130, 4096); memset(tv + 512 + 4096, 0, 128);
        const int* turb = cmds + tp + 2;
        for(int e = 0; e < 128; e++){
            const unsigned v = (unsigned)turb[e]; const int bp = 2 * (e & 63) + (e >> 6);
            tabs[bp] = (unsigned char)(v >> 16); tabs[128 + bp] = (unsigned char)v; tabs[256 + bp] = (unsigned char)(v >> 8);
        }
        X->staged = tp;
    }
    const HVX_Vector TH = *(const HVX_Vector*)tabs, T0 = *(const HVX_Vector*)(tabs + 128), T1 = *(const HVX_Vector*)(tabs + 256);
    const unsigned texv = (unsigned)(uintptr_t)(tv + 512);
    const HVX_Vector j = Q6_V_vand_VV(*(const HVX_Vector*)pq_lane_tab, Q6_V_vsplat_R(15));   /* j = lane & 15: the pixel within its segment */
    const HVX_VectorPred qlo = Q6_Q_vsetq_R(64);
    const HVX_Vector m127 = Q6_Vb_vsplat_R(127);
    const int* seg = cmds + pos + 7;
    unsigned char* pdest = fb + fb_off;
    for(int k = 0; k < nseg; k += 8){                            /* 8 segments = 128 pixels */
        HVX_Vector S[4], T[4];
        for(int w = 0; w < 4; w++){
            const int ka = k + 2 * w < nseg ? k + 2 * w : nseg - 1, kb = k + 2 * w + 1 < nseg ? k + 2 * w + 1 : nseg - 1;
            const int* ga = seg + 4 * ka; const int* gb = seg + 4 * kb;   /* clamped: those lanes are past count */
            const int s0 = ga[0], t0 = ga[1], ss0 = ga[2], ts0 = ga[3], s1 = gb[0], t1 = gb[1], ss1 = gb[2], ts1 = gb[3];
            const HVX_Vector SS = Q6_V_vmux_QVV(qlo, Q6_V_vsplat_R(ss0), Q6_V_vsplat_R(ss1));
            const HVX_Vector TS = Q6_V_vmux_QVV(qlo, Q6_V_vsplat_R(ts0), Q6_V_vsplat_R(ts1));
            HVX_Vector s = Q6_V_vmux_QVV(qlo, Q6_V_vsplat_R(s0), Q6_V_vsplat_R(s1));
            HVX_Vector t = Q6_V_vmux_QVV(qlo, Q6_V_vsplat_R(t0), Q6_V_vsplat_R(t1));
            s = Q6_Vw_vadd_VwVw(s, Q6_Vw_vmpyie_VwVuh(SS, j));   /* + j * sstep (mod 2^32, as the scalar += chain) */
            t = Q6_Vw_vadd_VwVw(t, Q6_Vw_vmpyie_VwVuh(TS, j));
            S[w] = s; T[w] = t;
        }
        const HVX_Vector sLa = Q6_Vh_vpacke_VwVw(S[1], S[0]), sLb = Q6_Vh_vpacke_VwVw(S[3], S[2]);
        const HVX_Vector sHa = Q6_Vh_vpacko_VwVw(S[1], S[0]), sHb = Q6_Vh_vpacko_VwVw(S[3], S[2]);
        const HVX_Vector tLa = Q6_Vh_vpacke_VwVw(T[1], T[0]), tLb = Q6_Vh_vpacke_VwVw(T[3], T[2]);
        const HVX_Vector tHa = Q6_Vh_vpacko_VwVw(T[1], T[0]), tHb = Q6_Vh_vpacko_VwVw(T[3], T[2]);
        const HVX_Vector s0b = Q6_Vb_vpacke_VhVh(sLb, sLa), s1b = Q6_Vb_vpacko_VhVh(sLb, sLa), s2b = Q6_Vb_vpacke_VhVh(sHb, sHa);
        const HVX_Vector t0b = Q6_Vb_vpacke_VhVh(tLb, tLa), t1b = Q6_Vb_vpacko_VhVh(tLb, tLa), t2b = Q6_Vb_vpacke_VhVh(tHb, tHa);
        const HVX_Vector A = Q6_V_vand_VV(t2b, m127), Bi = Q6_V_vand_VV(s2b, m127);
        const HVX_Vector sturb = pq_turb_add(s0b, s1b, s2b, pq_lut128(A, T0), pq_lut128(A, T1), pq_lut128(A, TH));
        const HVX_Vector tturb = pq_turb_add(t0b, t1b, t2b, pq_lut128(Bi, T0), pq_lut128(Bi, T1), pq_lut128(Bi, TH));
        const HVX_Vector hi = Q6_V_vand_VV(Q6_Vuh_vlsr_VuhR(tturb, 2), Q6_V_vsplat_R(0x3F3F3F3F));
        const HVX_Vector lo = Q6_V_vor_VV(Q6_Vh_vasl_VhR(Q6_V_vand_VV(tturb, Q6_V_vsplat_R(0x03030303)), 6), sturb);
        const HVX_VectorPair ix = Q6_W_vshuff_VVR(hi, lo, -1);
        const int p0 = 16 * k, n = count - p0 >= 128 ? 128 : count - p0;
        const int slot = X->chunk & 1; X->chunk++;
        HVX_Vector* g = (HVX_Vector*)(tv + 4736) + 2 * slot;
#ifndef PQ_TURB_DIAG_NOGATHER
        Q6_vgather_ARMVh(g, texv, 4096 + 127, Q6_V_lo_W(ix));
        if(n > 64) Q6_vgather_ARMVh(g + 1, texv, 4096 + 127, Q6_V_hi_W(ix));
#else
        g[0] = Q6_V_lo_W(ix); g[1] = Q6_V_hi_W(ix);
#endif
        pq_tfinish(X);                                           /* the previous round (the other slot) */
        X->active = 1; X->n = n; X->slot = slot; X->two = n > 64; X->dst = pdest + p0;
    }
    *npix += count;
    short* pz = zb + z_off;
#ifndef PQ_TURB_DIAG_NOZ
    if(!pq_z_hvx(pz, count, cmds[pos + 4], cmds[pos + 5])) pq_z_scalar(pz, count, (unsigned)cmds[pos + 4], (unsigned)cmds[pos + 5]);
#else
    (void)pz;
#endif
    return 1;
}
#endif
