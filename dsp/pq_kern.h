/* pq_kern.h - the per-span kernels of pxquake (2026-10-26, phase 3): the scalar reference and the HVX path.
 * Included by pxquake_imp.c (the skel) and pq_simtest.c (hexagon-sim, no NPU), so both run the same text.
 *
 * One span = the command record [2, fb_off, z_off, count, izi, izistep, ceil(count/8) x (s, t, sstep, tstep)].
 *
 * SCALAR (the phase-2 reference): per pixel  fb = tex[(s >> 16) + (t >> 16) * cw]; s += ss; t += ts;  z as id's
 * D_DrawZSpans including its 2-pixel packing.
 *
 * HVX TEXTURE (mode 1). Needs the surface block staged in VTCM (voff >= 0, vbytes <= 65536, cw <= 32767):
 *   lanes = segments: 32 segments per word vector. S, T, SS, TS de-interleaved from the stream by two vdeal levels.
 *   For j = 0..7: I_j = (S >> 16) + (T >> 16) * cw, then S += SS, T += TS  (j * ss by repeated addition, mod 2^32,
 *   exactly the scalar loop's arithmetic). Three vshuff levels (4, 8, 16 bytes) put the 8 x 32 indices in pixel order.
 *   Pairs of word vectors -> halfword offsets (vpacke) -> vgather .h from the VTCM block (offsets need not be aligned,
 *   the low byte of the gathered halfword is the texel) -> vpacke to bytes -> masked stores of exactly count bytes.
 *   Any valid pixel whose index is outside [0, vbytes) (the gather would drop it) marks the span: the scalar texture
 *   loop then redraws ALL its pixels (it reads the shared cache there, as the CPU does).
 * HVX Z (mode 1). When izi >= 0 and izi + (count-1) * izistep stays in [0, 2^31) (exact, 64-bit), the 32-bit values
 *   never wrap and are never negative, so id's pair packing equals per-pixel (izi_k >> 16): even/odd word vectors,
 *   vshuffo takes their upper halves in pixel order, 64 pixels per 128-byte masked store. Otherwise the scalar z.
 * Masked stores: byte-exact (vror into place, vsetq/vsetq2 predicates, two aligned predicated stores), so no byte
 * outside the span is written - spans of other threads may share the cache lines.
 */
#ifndef PQ_KERN_H
#define PQ_KERN_H
#include <stdint.h>
#include <hexagon_types.h>
#include <hexagon_protos.h>

typedef long pq_uvec __attribute__((__vector_size__(128))) __attribute__((aligned(4)));

typedef struct {
    const unsigned char* cache; int cacheLen; const unsigned char* vtcm;
    unsigned char* fb; int fbLen; short* zb; int zbLen;
    const int* cmds_end;              /* the stream must not be read past this (unaligned vector loads) */
    unsigned char* gs;                /* this thread's 256-byte gather target in VTCM (128-aligned) */
} pq_bufs;
typedef struct { int npix, oob, oobd, tex_hvx, tex_fallback, z_hvx, z_scalar; } pq_cnt;

/* ---------------------------------------------------------------- scalar reference */
static inline void pq_tex_scalar(const pq_bufs* B, const int* seg, int count, unsigned char* pdest, int base_off, int cw,
                                 const unsigned char* pv, int vbytes, int* oob){
    const unsigned char* pbase = B->cache + base_off;
    const int nseg = (count + 7) >> 3;
    int left = count;
    for(int q = 0; q < nseg; q++){
        int n = left >= 8 ? 8 : left; left -= n;
        int s = seg[4 * q], t = seg[4 * q + 1]; const int ss = seg[4 * q + 2], ts = seg[4 * q + 3];
        const long long sl = (long long)s + (long long)(n - 1) * ss, tl = (long long)t + (long long)(n - 1) * ts;
        const long long s0 = s < sl ? s : sl, s1 = s < sl ? sl : s, t0 = t < tl ? t : tl, t1 = t < tl ? tl : t;
        const long long lo = (s0 >> 16) + (t0 >> 16) * cw, hi = (s1 >> 16) + (t1 >> 16) * cw;
        if(pv && cw >= 0 && lo >= 0 && hi < vbytes && sl == (int)sl && tl == (int)tl){
            do { *pdest++ = pv[(s >> 16) + (t >> 16) * cw]; s += ss; t += ts; } while(--n > 0);
        } else if(cw >= 0 && base_off + lo >= 0 && base_off + hi < B->cacheLen && sl == (int)sl && tl == (int)tl){
            do { *pdest++ = pbase[(s >> 16) + (t >> 16) * cw]; s += ss; t += ts; } while(--n > 0);
        } else {
            do {
                const int idx = (s >> 16) + (t >> 16) * cw;
                if((unsigned)(base_off + idx) < (unsigned)B->cacheLen) *pdest = pbase[idx]; else (*oob)++;
                pdest++; s += ss; t += ts;
            } while(--n > 0);
        }
    }
}
static inline void pq_z_scalar(short* pz, int count, unsigned izi, unsigned izistep){
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

/* ---------------------------------------------------------------- HVX helpers */
/* store the first n bytes (1..128) of v at dst (any alignment), touching no other byte */
static inline void pq_st(void* dst, HVX_Vector v, int n){
    const uintptr_t a = (uintptr_t)dst; const int off = (int)(a & 127u);
    HVX_Vector* b = (HVX_Vector*)(a - (uintptr_t)off);
    const HVX_Vector r = Q6_V_vror_VR(v, (128 - off) & 127);
    const int end = off + n;
    const HVX_VectorPred qe = Q6_Q_vsetq2_R(end >= 128 ? 128 : end);       /* lanes < min(end,128) */
    Q6_vmem_QRIV(Q6_Q_and_QQn(qe, Q6_Q_vsetq_R(off)), b, r);               /* minus lanes < off */
    if(end > 128) Q6_vmem_QRIV(Q6_Q_vsetq_R(end - 128), b + 1, r);
}
static inline int pq_any(HVX_Vector acc){            /* 1 if any word is non-zero */
    HVX_Vector x = acc;
    x = Q6_V_vor_VV(x, Q6_V_vror_VR(x, 64));
    x = Q6_V_vor_VV(x, Q6_V_vror_VR(x, 32));
    x = Q6_V_vor_VV(x, Q6_V_vror_VR(x, 16));
    x = Q6_V_vor_VV(x, Q6_V_vror_VR(x, 8));
    x = Q6_V_vor_VV(x, Q6_V_vror_VR(x, 4));
    return Q6_R_vextract_VR(x, 0) != 0;
}
static const int pq_lane_tab[32] __attribute__((aligned(128))) = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31 };

#ifdef PQ_DEBUG
static unsigned short* pq_dbg; static int* pq_dbgS;
#endif
/* HVX texture of one span; returns 1 if a valid pixel's index was outside the block (caller redraws scalar) */
static inline int pq_tex_hvx_v1(const pq_bufs* B, const int* seg, int count, unsigned char* pdest, int cw,
                             const unsigned char* pv, int vbytes){
    const int nseg = (count + 7) >> 3;
    const int cw2 = (int)(((unsigned)cw & 0xFFFFu) * 0x10001u);   /* vmpyi(Vw, Rt.h) takes Rt.h[lane & 1] (sim: odd lanes were x0) */
    const HVX_Vector lim = Q6_V_vsplat_R(vbytes - 1);
    const HVX_Vector ones = Q6_V_vsplat_R(-1);
    HVX_Vector bad = Q6_V_vzero();
    HVX_Vector* gsv = (HVX_Vector*)B->gs;
    const HVX_Vector one16 = Q6_Vh_vsplat_R(1), even16 = Q6_Vh_vsplat_R(0xFFFE);
    int __attribute__((aligned(128))) tmp[128];
    for(int g = 0; g < nseg; g += 32){
        const int ng = nseg - g < 32 ? nseg - g : 32;
        const int* src = seg + 4 * g;
        HVX_Vector X0, X1, X2, X3;
        if(src + 128 <= B->cmds_end){
            X0 = *(const pq_uvec*)(src); X1 = *(const pq_uvec*)(src + 32); X2 = *(const pq_uvec*)(src + 64); X3 = *(const pq_uvec*)(src + 96);
        } else {                                     /* near the end of the stream: never read past it */
            for(int i = 0; i < 4 * ng; i++) tmp[i] = src[i];
            for(int i = 4 * ng; i < 128; i++) tmp[i] = 0;
            X0 = *(const HVX_Vector*)(tmp); X1 = *(const HVX_Vector*)(tmp + 32); X2 = *(const HVX_Vector*)(tmp + 64); X3 = *(const HVX_Vector*)(tmp + 96);
        }
        /* de-interleave [s t ss ts] x 32 -> S, T, SS, TS (lane = segment) */
        const HVX_VectorPair d01 = Q6_W_vdeal_VVR(X1, X0, -4);   /* lo: s ss (segs 0..15), hi: t ts */
        const HVX_VectorPair d23 = Q6_W_vdeal_VVR(X3, X2, -4);   /* segs 16..31 */
        const HVX_VectorPair dS = Q6_W_vdeal_VVR(Q6_V_lo_W(d23), Q6_V_lo_W(d01), -4);   /* lo: S, hi: SS */
        const HVX_VectorPair dT = Q6_W_vdeal_VVR(Q6_V_hi_W(d23), Q6_V_hi_W(d01), -4);   /* lo: T, hi: TS */
        HVX_Vector S = Q6_V_lo_W(dS), SS = Q6_V_hi_W(dS), T = Q6_V_lo_W(dT), TS = Q6_V_hi_W(dT);
#ifdef PQ_DEBUG
        if(pq_dbgS && g == 0){ memcpy(pq_dbgS, &S, 128); memcpy(pq_dbgS + 32, &SS, 128); memcpy(pq_dbgS + 64, &X0, 128); }
#endif
        HVX_Vector I[8];
        for(int j = 0; j < 8; j++){
            I[j] = Q6_Vw_vadd_VwVw(Q6_Vw_vasr_VwR(S, 16), Q6_Vw_vmpyi_VwRh(Q6_Vw_vasr_VwR(T, 16), cw2));
            S = Q6_Vw_vadd_VwVw(S, SS); T = Q6_Vw_vadd_VwVw(T, TS);
        }
        /* transpose to pixel order: P(j,j+1) at 4 bytes, then 8, then 16 */
        const HVX_VectorPair P01 = Q6_W_vshuff_VVR(I[1], I[0], -4), P23 = Q6_W_vshuff_VVR(I[3], I[2], -4);
        const HVX_VectorPair P45 = Q6_W_vshuff_VVR(I[5], I[4], -4), P67 = Q6_W_vshuff_VVR(I[7], I[6], -4);
        const HVX_VectorPair Xa = Q6_W_vshuff_VVR(Q6_V_lo_W(P23), Q6_V_lo_W(P01), -8);  /* j0..3, segs 0..7 | 8..15 */
        const HVX_VectorPair Xb = Q6_W_vshuff_VVR(Q6_V_hi_W(P23), Q6_V_hi_W(P01), -8);  /* j0..3, segs 16..23 | 24..31 */
        const HVX_VectorPair Ya = Q6_W_vshuff_VVR(Q6_V_lo_W(P67), Q6_V_lo_W(P45), -8);  /* j4..7 */
        const HVX_VectorPair Yb = Q6_W_vshuff_VVR(Q6_V_hi_W(P67), Q6_V_hi_W(P45), -8);
        HVX_VectorPair Q[4];
        Q[0] = Q6_W_vshuff_VVR(Q6_V_lo_W(Ya), Q6_V_lo_W(Xa), -16);   /* pixels 0..63 of the group */
        Q[1] = Q6_W_vshuff_VVR(Q6_V_hi_W(Ya), Q6_V_hi_W(Xa), -16);   /* 64..127 */
        Q[2] = Q6_W_vshuff_VVR(Q6_V_lo_W(Yb), Q6_V_lo_W(Xb), -16);   /* 128..191 */
        Q[3] = Q6_W_vshuff_VVR(Q6_V_hi_W(Yb), Q6_V_hi_W(Xb), -16);   /* 192..255 */
        const int gpix = 8 * g;                                      /* first pixel of this group in the span */
        for(int c = 0; c < 4; c += 2){                               /* 128 pixels per output vector */
            const int p0 = gpix + 64 * c;
            if(p0 >= count) break;
            HVX_Vector G[2]; HVX_VectorPred odd[2]; odd[0] = odd[1] = Q6_Q_vsetq_R(0);
            for(int h = 0; h < 2; h++){                             /* 1: every valid index inside the block? */
                const int pb = p0 + 64 * h;
                const HVX_Vector W0 = Q6_V_lo_W(Q[c + h]), W1 = Q6_V_hi_W(Q[c + h]);
                int nv0 = count - pb, nv1 = count - pb - 32;          /* valid lanes in W0 / W1 */
                if(nv0 > 0){ if(nv0 > 32) nv0 = 32;
                    bad = Q6_V_vmux_QVV(Q6_Q_and_QQ(Q6_Q_vcmp_gt_VuwVuw(W0, lim), Q6_Q_vsetq2_R(4 * nv0)), ones, bad); }
                if(nv1 > 0){ if(nv1 > 32) nv1 = 32;
                    bad = Q6_V_vmux_QVV(Q6_Q_and_QQ(Q6_Q_vcmp_gt_VuwVuw(W1, lim), Q6_Q_vsetq2_R(4 * nv1)), ones, bad); }
            }
            if(pq_any(bad)) return 1;                                /* nothing of this chunk written yet */
            for(int h = 0; h < 2; h++){                             /* 2: gather */
                const int pb = p0 + 64 * h;
                if(pb >= count) break;
                const HVX_Vector off = Q6_Vh_vpacke_VwVw(Q6_V_hi_W(Q[c + h]), Q6_V_lo_W(Q[c + h]));
#ifdef PQ_DEBUG
                if(pq_dbg) memcpy(pq_dbg + pb, &off, 128);
#endif
#ifdef PQ_GATHER_ODD_DIRECT
                Q6_vgather_ARMVh(gsv + h, (unsigned)(uintptr_t)pv, vbytes - 1, off);
#else
                /* gather the aligned halfword at idx & ~1 and pick the byte by idx & 1 */
                odd[h] = Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(off, one16), one16);
                Q6_vgather_ARMVh(gsv + h, (unsigned)(uintptr_t)pv, vbytes - 1, Q6_V_vand_VV(off, even16));
#endif
            }
            G[0] = gsv[0];
            G[1] = (p0 + 64 < count) ? gsv[1] : Q6_V_vzero();
            G[0] = Q6_V_vmux_QVV(odd[0], Q6_Vuh_vlsr_VuhR(G[0], 8), G[0]);
            G[1] = Q6_V_vmux_QVV(odd[1], Q6_Vuh_vlsr_VuhR(G[1], 8), G[1]);
            const int n = count - p0 >= 128 ? 128 : count - p0;
            pq_st(pdest + p0, Q6_Vb_vpacke_VhVh(G[1], G[0]), n);
        }
    }
    return pq_any(bad);
}

#include "pq_tex2.h"   /* pq_tex_hvx: version 2 (pass A bounds, lagged gathers; 512 B of gather targets per thread) */

/* HVX z of one span; returns 0 if it could not be used (caller does the scalar z) */
static inline int pq_z_hvx(short* pz, int count, int izi, int izistep){
    const long long last = (long long)izi + (long long)(count - 1) * (long long)izistep;
    if(izi < 0 || last < 0 || last > 0x7FFFFFFFLL) return 0;
    const HVX_Vector lane = *(const HVX_Vector*)pq_lane_tab;
    /* E: word i = izi + 2i*step, O = E + step (no wrap: proven above) */
    /* lane * st2 (mod 2^32) as lane * hi << 16 + lane * lo, lo = the signed low halfword: two vmpyi by halfwords
     * (vmpyi(Vw, Rt.h) takes Rt.h[lane & 1], so each halfword is replicated) */
    const unsigned st2 = 2u * (unsigned)izistep;
    const int lo = (int)(short)(st2 & 0xFFFFu), hi = (int)((st2 - (unsigned)lo) >> 16);
    const HVX_Vector mlo = Q6_Vw_vmpyi_VwRh(lane, (int)(((unsigned)lo & 0xFFFFu) * 0x10001u));
    const HVX_Vector mhi = Q6_Vw_vmpyi_VwRh(lane, (int)(((unsigned)hi & 0xFFFFu) * 0x10001u));
    HVX_Vector E = Q6_Vw_vadd_VwVw(Q6_V_vsplat_R(izi), Q6_Vw_vadd_VwVw(mlo, Q6_Vw_vasl_VwR(mhi, 16)));
    HVX_Vector O = Q6_Vw_vadd_VwVw(E, Q6_V_vsplat_R(izistep));
    const HVX_Vector step64 = Q6_V_vsplat_R((int)(64u * (unsigned)izistep));
    unsigned char* d = (unsigned char*)pz;
    for(int p = 0; p < count; p += 64){
        const int n = count - p >= 64 ? 64 : count - p;
        pq_st(d + 2 * p, Q6_Vh_vshuffo_VhVh(O, E), 2 * n);
        E = Q6_Vw_vadd_VwVw(E, step64); O = Q6_Vw_vadd_VwVw(O, step64);
    }
    return 1;
}

/* one span, any mode. mode 0 = scalar, 1 = HVX (texture needs the staged block, z needs the no-wrap proof) */
static inline void pq_span(const pq_bufs* B, const int* sp, int base_off, int cw, int voff, int vbytes, int mode, int diag, pq_cnt* C){
    const int fb_off = sp[1], z_off = sp[2], count = sp[3];
    const int* seg = sp + 6;
    if(fb_off < 0 || fb_off + count > B->fbLen || z_off < 0 || z_off + count > B->zbLen){ C->oobd++; return; }
    const unsigned char* pv = voff >= 0 ? B->vtcm + voff : 0;
    unsigned char* pdest = B->fb + fb_off;
    if(!(diag & 1)){
        if(mode == 1 && pv && vbytes > 0 && vbytes <= 65536 && cw >= 0 && cw <= 32767){
            if(pq_tex_hvx(B, seg, count, pdest, cw, pv, vbytes)){ C->tex_fallback++; pq_tex_scalar(B, seg, count, pdest, base_off, cw, pv, vbytes, &C->oob); }
            else C->tex_hvx++;
        } else pq_tex_scalar(B, seg, count, pdest, base_off, cw, pv, vbytes, &C->oob);
    }
    C->npix += count;
    if(diag & 2) return;
    short* pz = B->zb + z_off;
    if(mode == 1 && pq_z_hvx(pz, count, sp[4], sp[5])) C->z_hvx++;
    else { pq_z_scalar(pz, count, (unsigned)sp[4], (unsigned)sp[5]); C->z_scalar++; }
}
#include "pq_tex3.h"
#include "pq_tex4.h"
/* one span with the cross-span gather pipeline (v2 band, simtest). Texture fallbacks/oob are counted in P. */
static inline void pq_span_p(const pq_bufs* B, pq_pipe4* P, const int* sp, int base_off, int cw, int voff, int vbytes, int diag, pq_cnt* C){
    const int fb_off = sp[1], z_off = sp[2], count = sp[3];
    const int* seg = sp + 6;
    if(fb_off < 0 || fb_off + count > B->fbLen || z_off < 0 || z_off + count > B->zbLen){ C->oobd++; return; }
    const unsigned char* pv = voff >= 0 ? B->vtcm + voff : 0;
    unsigned char* pdest = B->fb + fb_off;
    if(!(diag & 1)){
        if(pv && vbytes > 0 && vbytes <= 65536 && cw >= 0 && cw <= 32767){ pq_tex_hvx4(B, P, seg, count, pdest, base_off, cw, pv, vbytes); C->tex_hvx++; }
        else pq_tex_scalar(B, seg, count, pdest, base_off, cw, pv, vbytes, &C->oob);
    }
    C->npix += count;
    if(diag & 2) return;
    short* pz = B->zb + z_off;
    if(pq_z_hvx(pz, count, sp[4], sp[5])) C->z_hvx++;
    else { pq_z_scalar(pz, count, (unsigned)sp[4], (unsigned)sp[5]); C->z_scalar++; }
}
static inline void pq_span_p_end(const pq_bufs* B, pq_pipe4* P, pq_cnt* C){
    pq_pipe4_end(B, P); C->tex_fallback += P->fallbacks; C->tex_hvx -= P->fallbacks; C->oob += P->oob; P->fallbacks = 0; P->oob = 0;
}
#include "pq_tex5.h"
/* version 5 entry points (v2 band, simtest): texture by pq_tex_hvx5; after the thread's spans pq_span_p5_end says
 * whether any pixel was out of its block - then pq_span_tex_redo for every span of the thread */
static inline void pq_span_p5(const pq_bufs* B, pq_pipe5* P, const int* sp, int base_off, int cw, int voff, int vbytes, int diag, pq_cnt* C){
    const int fb_off = sp[1], z_off = sp[2], count = sp[3];
    const int* seg = sp + 6;
    if(fb_off < 0 || fb_off + count > B->fbLen || z_off < 0 || z_off + count > B->zbLen){ C->oobd++; return; }
    const unsigned char* pv = voff >= 0 ? B->vtcm + voff : 0;
    unsigned char* pdest = B->fb + fb_off;
    if(!(diag & 1)){
        if(pv && vbytes > 0 && vbytes <= 65536 && cw >= 0 && cw <= 32767){ pq_tex_hvx5(B, P, seg, count, pdest, cw, pv, vbytes); C->tex_hvx++; }
        else pq_tex_scalar(B, seg, count, pdest, base_off, cw, pv, vbytes, &C->oob);
    }
    C->npix += count;
    if(diag & 2) return;
    short* pz = B->zb + z_off;
    if(pq_z_hvx(pz, count, sp[4], sp[5])) C->z_hvx++;
    else { pq_z_scalar(pz, count, (unsigned)sp[4], (unsigned)sp[5]); C->z_scalar++; }
}
static inline void pq_span_tex_redo(const pq_bufs* B, const int* sp, int base_off, int cw, int voff, int vbytes, pq_cnt* C){
    const int fb_off = sp[1], z_off = sp[2], count = sp[3];
    if(fb_off < 0 || fb_off + count > B->fbLen || z_off < 0 || z_off + count > B->zbLen) return;
    const unsigned char* pv = voff >= 0 ? B->vtcm + voff : 0;
    pq_tex_scalar(B, sp + 6, count, B->fb + fb_off, base_off, cw, pv, vbytes, &C->oob);
    C->tex_fallback++;
}
#endif
