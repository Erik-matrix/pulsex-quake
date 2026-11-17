/*
pc_pipe.c - PulseX Quake phase 4 (2026-10-26): the frame pipeline, -dspspans 3.

Frame N, in program order (Quake's order is unchanged; what moves is WHEN things touch which buffer):
  SCR_UpdateScreen ... pre-3D 2D (tile clears)  -> into F (the persistent screen); before it: frame N-1 is finished
  R_RenderView begin    vid.buffer = W[b], z = Z[b] (b = N & 1): the world goes there (CPU sky/water, DSP spans)
  end of R_EdgeDrawing  frame N-1 is finished (its DSP job is waited for), then frame N's spans go to the DSP as an
                        ASYNCHRONOUS job (bridge/pxquake: a worker thread owns the session)
  models, particles,    RECORDED, not drawn: every pixel candidate (offset, z, colour, comparison) in program order;
  sprites, view model   they cannot test against z while the DSP is still writing it
  R_RenderView end      vid.buffer = F again; post-3D 2D (crosshair, status bar, console, menu) goes straight into F,
                        and a MASK marks those pixels
  VID_Update, dump      deferred: frame N is "pending"
  ... frame N+1: host, game logic, world pass (bsp, edges, surface cache, span queue) run WHILE the DSP draws N ...
  finish(N)             wait for the DSP job; replay the records on W[b]/Z[b] (the same tests in the same order as
                        Quake would have done them there - exact by construction); copy the view rectangle W[b] -> F
                        except masked pixels; present F and write the dump of frame N.
Exactness: every pixel of F gets, in the end, the value Quake's single buffer would hold: view pixels from the world
(DSP + CPU) with the models replayed on the real z; 2D pixels drawn after the 3D view win over it (mask); 2D drawn
before the view (tile clears) is overwritten by the view copy where Quake's view overwrote it; pixels nobody writes
keep their old value (F is persistent, as vid.buffer is).
Anything that READS the screen in the middle of a frame (Draw_FadeScreen, screenshots) first drains the pipeline.
*/
#include "quakedef.h"
#include "d_local.h"
#include "r_local.h"
#include "pc_dsp.h"
#include "pc_pipe.h"
#include <windows.h>

int				pc_pipe_on;
int				pc_pipe_sick = -1;	// -dsppipesick N
pc_rec_t		*pc_rec;
int				pc_rec_n, pc_rec_cap;
unsigned char	*pc_mask;
unsigned char	*pc_maskrow;
int				pc_mask_lo = 0x7FFFFFFF, pc_mask_hi = -1;

extern unsigned char	pc_palette[768];
extern short	*zspantable[MAXHEIGHT];
void	PC_HarnessDumpFrom (int index, const byte *buf, const unsigned char *pal);
void	PC_VidPresentFrom (const byte *buf);

static byte		*pF, *pW[2];
static short	*pFz, *pZ[2];
static unsigned char	*mask_buf;
static pc_rec_t	*recbuf[2];
static int		pw, ph;
static int		frame_id;
static int		cur_view, cur_b, cur_job;
static struct {
	int		active, id, b, job, nrec, dump;
	unsigned	last_seq;	// host sequence number of the frame's last DSP job
	vrect_t	vrect;
	unsigned char	pal[768];
} pend;
static long		st_fin_world, st_fin_2d, st_fin_viewend;
long			pc_pre3d_skipped;
static int		in_worlddone;
int			pc_2d_line;
static long		st_line[4096];
static long		st_frames, st_jobs, st_finishes, st_drains, st_overflows, st_recs, st_recmax, st_before2d;
static double	st_finish_ms, st_replay_ms, st_copy_ms;
static LARGE_INTEGER	qf;
static double now_ms (void) { LARGE_INTEGER c; if (!qf.QuadPart) QueryPerformanceFrequency (&qf); QueryPerformanceCounter (&c); return 1000.0 * (double)c.QuadPart / (double)qf.QuadPart; }

static void set_zbuffer (short *z)
{
	int		i;
	d_pzbuffer = z;
	for (i = 0 ; i < (int)vid.height ; i++)
		zspantable[i] = d_pzbuffer + i * d_zwidth;
}

void PC_PipeInit (byte *F, short *Fz, byte *w0, short *z0, byte *w1, short *z1, int w, int h)
{
	pF = F; pFz = Fz; pW[0] = w0; pZ[0] = z0; pW[1] = w1; pZ[1] = z1; pw = w; ph = h;
	mask_buf = calloc (1, (size_t)w * h);
	pc_maskrow = calloc (1, (size_t)h + 16);
	pc_rec_cap = 1 << 21;			// 2 M candidates per frame (24 MB per buffer); more = overflow path
	recbuf[0] = malloc (sizeof (pc_rec_t) * (size_t)pc_rec_cap);
	recbuf[1] = malloc (sizeof (pc_rec_t) * (size_t)pc_rec_cap);
	if (!mask_buf || !pc_maskrow || !recbuf[0] || !recbuf[1])
		Sys_Error ("pipeline: out of memory");
	pc_pipe_on = 1;
}

static void replay (pc_rec_t *r, int n, byte *fb, short *zb)
{
	int		i;
	for (i = 0 ; i < n ; i++, r++)
	{
		short	*pz = zb + r->ofs;
		if (r->op ? (r->z > *pz) : (r->z >= *pz))
		{
			*pz = (short)r->z;
			fb[r->ofs] = r->pix;
		}
	}
}

void PC_RecOverflow (int ofs, int z, int pix, int op)
{
	// more candidates than the buffer holds: the DSP job of this frame is waited for, what was recorded is applied,
	// and the rest of the frame's models draw directly (the z is final now) - same tests, same order
	pc_rec_t	one;
	double		w = pcdsp_wait ();
	pcdsp_note_wait (w);
	replay (recbuf[cur_b], pc_rec_n, pW[cur_b], pZ[cur_b]);
	st_recs += pc_rec_n;
	pc_rec = NULL;
	pc_rec_n = 0;
	one.ofs = ofs; one.z = z; one.pix = (unsigned char)pix; one.op = (unsigned char)op;
	replay (&one, 1, pW[cur_b], pZ[cur_b]);
	st_overflows++;
}

#define MAXDEFER 64
static struct { vrect_t r; int rowbytes; byte *psrc; int transparent; } defer[MAXDEFER];
static int		ndefer, in_defer;
static long		st_deferred;
void R_DrawRect8 (vrect_t *prect, int rowbytes, byte *psrc, int transparent);
int PC_PipeDeferRect (vrect_t *prect, int rowbytes, byte *psrc, int transparent)
{
	if (in_defer || !pend.active || pend.id >= frame_id || ndefer >= MAXDEFER)
		return 0;	// nothing pending (or the list is full): draw now
	defer[ndefer].r = *prect; defer[ndefer].r.pnext = NULL; defer[ndefer].rowbytes = rowbytes; defer[ndefer].psrc = psrc; defer[ndefer].transparent = transparent;
	ndefer++; st_deferred++;
	return 1;
}
static void run_deferred (void)
{
	int	i;
	in_defer = 1;
	for (i = 0 ; i < ndefer ; i++)
		R_DrawRect8 (&defer[i].r, defer[i].rowbytes, defer[i].psrc, defer[i].transparent);
	ndefer = 0;
	in_defer = 0;
}

static void finish (void)
{
	double	t0 = now_ms (), t1, t2;
	int		x, y, b;
	if (!pend.active)
		return;
	b = pend.b;
	if (pend.job && pcdsp_inflight ())
		pcdsp_note_wait (pcdsp_wait ());
	t1 = now_ms ();
	if (pc_pipe_sick >= 0 && pend.id >= pc_pipe_sick && pend.nrec > 0)	// known-sick control: one candidate dropped
		replay (recbuf[b] + 1, pend.nrec - 1, pW[b], pZ[b]);
	else
	replay (recbuf[b], pend.nrec, pW[b], pZ[b]);
	st_recs += pend.nrec;
	if (pend.nrec > st_recmax) st_recmax = pend.nrec;
	t2 = now_ms ();
	// the view rectangle W[b] -> F, except the 2D pixels drawn after the view (mask)
	{
		const int	rb = vid.rowbytes;
		const int	y0 = pend.vrect.y, y1 = pend.vrect.y + pend.vrect.height;
		const int	x0 = pend.vrect.x, x1 = pend.vrect.x + pend.vrect.width;
		const int	mrow0 = pc_mask_hi >= 0 ? pc_mask_lo / rb : 1 << 30, mrow1 = pc_mask_hi >= 0 ? pc_mask_hi / rb : -1;
		for (y = y0 ; y < y1 ; y++)
		{
			byte	*d = pF + y * rb, *s = pW[b] + y * rb;
			if (y < mrow0 || y > mrow1 || !pc_maskrow[y])
				memcpy (d + x0, s + x0, x1 - x0);
			else
			{
				const unsigned char	*m = mask_buf + y * rb;
				for (x = x0 ; x < x1 ; x++)
					if (!m[x])
						d[x] = s[x];
			}
		}
		if (pc_mask_hi >= 0)
		{
			memset (mask_buf + pc_mask_lo, 0, pc_mask_hi - pc_mask_lo + 1);
			memset (pc_maskrow + mrow0, 0, mrow1 - mrow0 + 1);
		}
		pc_mask_lo = 0x7FFFFFFF; pc_mask_hi = -1;
	}
	st_copy_ms += now_ms () - t2;
	st_replay_ms += t2 - t1;
	pend.active = 0;
	PC_VidPresentFrom (pF);
	if (pend.dump >= 0)
		PC_HarnessDumpFrom (pend.dump, pF, pend.pal);
	if (ndefer)
	{
		byte	*save = vid.buffer;
		unsigned char	*savem = pc_mask;
		vid.buffer = pF;
		pc_mask = NULL;	// pre-3D 2D of the next frame: not masked (its view overwrites it)
		run_deferred ();
		vid.buffer = save;
		pc_mask = savem;
	}
	st_finishes++;
	st_finish_ms += now_ms () - t0;
}

void PC_PipeFrameBegin (void)
{
	if (!pc_pipe_on)
		return;
	frame_id++;
	pc_mask = NULL;		// 2D before the view is not masked: the view overwrites it, as in Quake
}

void PC_PipeBefore2D (void)
{
	// any 2D write of a LATER frame needs the earlier frame finished first (its view copy would land on top)
	if (pend.active && pend.id < frame_id)
	{
		st_before2d++;
		if (in_worlddone) st_fin_world++; else { st_fin_2d++; if (pc_2d_line > 0 && pc_2d_line < 4096) st_line[pc_2d_line]++; }
		finish ();
	}
}

void PC_PipeViewBegin (void)
{
	if (!pc_pipe_on)
		return;
	if (lcd_x.value)		// the interleaved stereo view writes vid.buffer + rowbytes/2: not pipelined
	{
		PC_PipeDrain ();
		cur_view = 0;
		return;
	}
	if (pend.active && pend.id < frame_id - 1)
		finish ();
	cur_b = frame_id & 1;
	cur_view = 1;
	cur_job = 0;
	vid.buffer = pW[cur_b];
	set_zbuffer (pZ[cur_b]);
	pcdsp_set_target (pW[cur_b], pZ[cur_b]);
	pcdsp_set_async_ok (1);	// phase 5: mid-frame flushes may run asynchronously (mailbox mode)
	st_frames++;
}

void PC_PipeWorldDone (void)
{
	if (!pc_pipe_on || !cur_view)
	{
		pcdsp_frame_end ();
		return;
	}
	pcdsp_set_async_ok (0);
	// 1. frame N-1's DSP job(s): waited for (its world z/fb are final now)
	if (pend.active && pend.id < frame_id && pend.job && pcdsp_inflight ())
		pcdsp_note_wait (pcdsp_wait_upto (pend.last_seq));	// this frame's own partial jobs keep running
	if (pend.active)
		pend.job = 0;
	// 2. frame N's job launched at once
	if (pcdsp_queued ())
	{
		cur_job = 1;
		st_jobs++;
		pcdsp_frame_end_async ();
	}
	else
		pcdsp_frame_end_async ();	// counts the frame, launches nothing
	// 3. the CPU part of finishing N-1 (replay, view copy, present, dump) while the DSP draws N
	in_worlddone = 1;
	PC_PipeBefore2D ();
	in_worlddone = 0;
	if (cur_job)
	{
		pc_rec = recbuf[cur_b];
		pc_rec_n = 0;
	}
}

void PC_PipeViewEnd (void)
{
	if (!pc_pipe_on || !cur_view)
		return;
	if (pend.active)
		st_fin_viewend++;
	if (pend.active)
		finish ();				// an older frame still open (no world pass ran this frame): before any 2D
	pend.active = 1;
	pend.id = frame_id;
	pend.b = cur_b;
	pend.job = cur_job && pcdsp_inflight ();
	pend.last_seq = pcdsp_last_seq ();
	pend.nrec = pc_rec ? pc_rec_n : 0;
	pend.vrect = scr_vrect;		// the view on the SCREEN (in warp frames r_refdef.vrect is the warp buffer)
	pend.dump = -1;
	pc_rec = NULL;
	pc_rec_n = 0;
	cur_view = 0;
	vid.buffer = pF;
	set_zbuffer (pFz);
	pcdsp_set_target (pF, pFz);
	pc_mask = mask_buf;
}

void PC_PipeDrain (void)
{
	if (!pc_pipe_on)
		return;
	if (pend.active)
	{
		st_drains++;
		finish ();
	}
	pc_mask = NULL;
}

// PulseX phase 9: the DSP is lost - finish what is pending and render the rest of the session like -dspspans 0
void PC_PipeDisable (void)
{
	if (!pc_pipe_on)
		return;
	PC_PipeDrain ();
	pc_pipe_on = 0;
	pc_mask = NULL;
	pc_rec = NULL;
	pc_rec_n = 0;
	vid.buffer = pF;
	set_zbuffer (pFz);
}

int PC_PipeCurrentPending (void)
{
	return pc_pipe_on && pend.active && pend.id == frame_id;
}

void PC_PipeRequestDump (int index)
{
	pend.dump = index;
	memcpy (pend.pal, pc_palette, 768);
}

void PC_PipeStats (void)
{
	if (!pc_pipe_on)
		return;
	printf ("PCPIPE finish_at_worlddone=%ld finish_at_2d=%ld finish_at_viewend=%ld pre3d_clears_skipped=%ld pre3d_clears_deferred=%ld\n", st_fin_world, st_fin_2d, st_fin_viewend, pc_pre3d_skipped, st_deferred);
	{ int i; for (i = 0 ; i < 4096 ; i++) if (st_line[i]) printf ("PCPIPE finish_from_draw_line=%d count=%ld\n", i, st_line[i]); }
	printf ("PCPIPE frames=%ld jobs=%ld finishes=%ld finish_before_2d=%ld drains=%ld overflows=%ld records_per_frame=%.0f records_max=%ld finish_ms_per_frame=%.4f replay_ms_per_frame=%.4f copy_ms_per_frame=%.4f\n",
		st_frames, st_jobs, st_finishes, st_before2d, st_drains, st_overflows, st_finishes ? (double)st_recs / st_finishes : 0.0, st_recmax,
		st_finishes ? st_finish_ms / st_finishes : 0.0, st_finishes ? st_replay_ms / st_finishes : 0.0, st_finishes ? st_copy_ms / st_finishes : 0.0);
	fflush (stdout);
}
