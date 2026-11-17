/*
d_dspspans.c - PulseX Quake phase 2 (2026-10-26): the CPU half of span drawing on the Hexagon cDSP.

For one normal (surface-cached) world surface this does EXACTLY the floating point of id's D_DrawSpans8 and
D_DrawZSpans (d_scan.c) - the same expressions, the same types, compiled with the same flags (-ffp-contract=off) -
but instead of stepping pixels it writes the integer results into the frame's span queue (bridge/pxquake/pc_dsp.c):
per span the destination, count, izi and izistep; per 8-pixel segment s, t, sstep, tstep. The DSP
(skel/pxquake/pxquake_imp.c) then does only the integer stepping, texel fetches and z writes, in ONE call per frame
(pcdsp_frame_end at the end of R_EdgeDrawing). Integer arithmetic has one answer, so the result is bit-identical
by construction; the framedump trees prove it.

Selected with -dspspans 1. With -dspsick <frame> the first segment of the first span of every frame from that frame
on gets sstep += 1.0 (one texel per pixel): the known-sick control.
*/
#include "quakedef.h"
#include "d_local.h"
#include "r_local.h"		// SPEED (phase 6)
#include "pc_dsp.h"

int pcdsp_sick_now (void);

void PC_DspQueueSurface (espan_t *pspan, surfcache_t *pcache)
{
	int				count, spancount, nseg, *seg, izi, izistep;
	unsigned char	*pdest;
	short			*pz;
	fixed16_t		s, t, snext, tnext, sstep, tstep;
	float			sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float			sdivz8stepu, tdivz8stepu, zi8stepu;
	double			zid;

	sstep = 0;	// keep compiler happy
	tstep = 0;	// ditto

	pcdsp_q_surface ((unsigned char *)cacheblock, cachewidth, (unsigned char *)pcache, (unsigned char *)pcache + pcache->size);

	sdivz8stepu = d_sdivzstepu * 8;
	tdivz8stepu = d_tdivzstepu * 8;
	zi8stepu = d_zistepu * 8;

	// D_DrawZSpans: once per surface
	izistep = (int)(d_zistepu * 0x8000 * 0x10000);

	do
	{
		pdest = (unsigned char *)((byte *)d_viewbuffer +
				(screenwidth * pspan->v) + pspan->u);
		pz = d_pzbuffer + (d_zwidth * pspan->v) + pspan->u;

		count = pspan->count;

	// calculate the initial s/z, t/z, 1/z, s, and t and clamp
		du = (float)pspan->u;
		dv = (float)pspan->v;

		// D_DrawZSpans' own initial 1/z (float expression into a double, as there)
		zid = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		izi = (int)(zid * 0x8000 * 0x10000);

		seg = pcdsp_q_span (pdest, pz, count, izi, izistep);
		nseg = 0;

		sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

		s = (int)(sdivz * z) + sadjust;
		if (s > bbextents)
			s = bbextents;
		else if (s < 0)
			s = 0;

		t = (int)(tdivz * z) + tadjust;
		if (t > bbextentt)
			t = bbextentt;
		else if (t < 0)
			t = 0;

		do
		{
		// calculate s and t at the far end of the span
			if (count >= 8)
				spancount = 8;
			else
				spancount = count;

			count -= spancount;

			if (count)
			{
				sdivz += sdivz8stepu;
				tdivz += tdivz8stepu;
				zi += zi8stepu;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8;

				sstep = (snext - s) >> 3;
				tstep = (tnext - t) >> 3;
			}
			else
			{
				spancountminus1 = (float)(spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8;

				if (spancount > 1)
				{
					sstep = (snext - s) / (spancount - 1);
					tstep = (tnext - t) / (spancount - 1);
				}
			}

			// the segment: start s, t and the steps (id's inner loop runs on the DSP)
			seg[4*nseg+0] = s;
			seg[4*nseg+1] = t;
			seg[4*nseg+2] = sstep;
			seg[4*nseg+3] = tstep;
			nseg++;

			s = snext;
			t = tnext;

		} while (count > 0);

		if (pcdsp_sick_now ())
			seg[2] += 0x10000;		// known-sick control: one texel further per pixel in this segment

	} while ((pspan = pspan->pnext) != NULL);
}

/*
PulseX phase 6: Turbulent8 (d_scan.c) + D_DrawZSpans for one water/slime/lava/teleport surface. The float part is id's,
expression for expression; the DSP gets per span dest, z, count, izi, izistep and per 16-pixel segment s, t (masked
with (CYCLE<<16)-1 exactly where id masks them, before the pixel loop), sstep, tstep. The texture (cacheblock, 64x64)
and r_turb_turb's 128 ints go into the stream once per texture.
*/
int pc_turb_pix_frame, pc_turb_dsp_frame;

void PC_DspQueueTurb (espan_t *pspan)
{
	int				count, spancount, nseg, *seg, izi, izistep;
	unsigned char	*pdest;
	short			*pz;
	fixed16_t		s, t, snext, tnext, sstep, tstep;
	float			sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float			sdivz16stepu, tdivz16stepu, zi16stepu;
	double			zid;

	pcdsp_q_turb ((const unsigned char *)cacheblock, sintable + ((int)(cl.time*SPEED)&(CYCLE-1)));

	sstep = 0;	// keep compiler happy
	tstep = 0;	// ditto

	sdivz16stepu = d_sdivzstepu * 16;
	tdivz16stepu = d_tdivzstepu * 16;
	zi16stepu = d_zistepu * 16;

	// D_DrawZSpans: once per surface
	izistep = (int)(d_zistepu * 0x8000 * 0x10000);

	do
	{
		pdest = (unsigned char *)((byte *)d_viewbuffer +
				(screenwidth * pspan->v) + pspan->u);
		pz = d_pzbuffer + (d_zwidth * pspan->v) + pspan->u;

		count = pspan->count;

	// calculate the initial s/z, t/z, 1/z, s, and t and clamp
		du = (float)pspan->u;
		dv = (float)pspan->v;

		zid = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		izi = (int)(zid * 0x8000 * 0x10000);

		seg = pcdsp_q_turbspan (pdest, pz, count, izi, izistep);
		nseg = 0;
		pc_turb_dsp_frame += count;

		sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

		s = (int)(sdivz * z) + sadjust;
		if (s > bbextents)
			s = bbextents;
		else if (s < 0)
			s = 0;

		t = (int)(tdivz * z) + tadjust;
		if (t > bbextentt)
			t = bbextentt;
		else if (t < 0)
			t = 0;

		do
		{
		// calculate s and t at the far end of the span
			if (count >= 16)
				spancount = 16;
			else
				spancount = count;

			count -= spancount;

			if (count)
			{
				sdivz += sdivz16stepu;
				tdivz += tdivz16stepu;
				zi += zi16stepu;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 16)
					snext = 16;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 16)
					tnext = 16;

				sstep = (snext - s) >> 4;
				tstep = (tnext - t) >> 4;
			}
			else
			{
				spancountminus1 = (float)(spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 16)
					snext = 16;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 16)
					tnext = 16;

				if (spancount > 1)
				{
					sstep = (snext - s) / (spancount - 1);
					tstep = (tnext - t) / (spancount - 1);
				}
			}

			// id masks r_turb_s/t here, just before D_DrawTurbulent8Span
			seg[4*nseg+0] = s & ((CYCLE<<16)-1);
			seg[4*nseg+1] = t & ((CYCLE<<16)-1);
			seg[4*nseg+2] = sstep;
			seg[4*nseg+3] = tstep;
			nseg++;

			s = snext;
			t = tnext;

		} while (count > 0);

	} while ((pspan = pspan->pnext) != NULL);
}
