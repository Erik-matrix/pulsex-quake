/* pc_pipe.h - PulseX Quake phase 4: the frame pipeline for -dspspans 3 (pc/pc_pipe.c). */
#ifndef PC_PIPE_H
#define PC_PIPE_H

extern int		pc_pipe_on;				// -dspspans 3

/* model / particle / sprite pixel candidates of a pipelined frame, replayed on the real z once the DSP is done */
typedef struct { int ofs; int z; unsigned char pix, op; unsigned short pad; } pc_rec_t;
extern pc_rec_t	*pc_rec;				// non-NULL = record mode
extern int		pc_rec_n, pc_rec_cap;
void	PC_RecOverflow (int ofs, int z, int pix, int op);
static inline void PC_Rec (int ofs, int z, int pix, int op)
{
	if (pc_rec_n < pc_rec_cap)
	{
		pc_rec_t *r = &pc_rec[pc_rec_n++];
		r->ofs = ofs; r->z = z; r->pix = (unsigned char)pix; r->op = (unsigned char)op;
	}
	else
		PC_RecOverflow (ofs, z, pix, op);
}

/* 2D: before any 2D write; and the mask of 2D pixels that the deferred view copy must not overwrite */
extern unsigned char	*pc_mask;		// non-NULL = post-3D 2D of a pending frame
extern int		pc_mask_lo, pc_mask_hi;
void	PC_PipeBefore2D (void);
int		PC_PipeDeferRect (vrect_t *prect, int rowbytes, byte *psrc, int transparent);	// 1 = deferred
extern unsigned char	*pc_maskrow;	// per row: 2D drawn there
static inline void PC_Mask1 (int off)
{
	pc_mask[off] = 1;
	pc_maskrow[off / (int)vid.rowbytes] = 1;
	if (off < pc_mask_lo) pc_mask_lo = off;
	if (off > pc_mask_hi) pc_mask_hi = off;
}
static inline void PC_MaskN (int off, int n)
{
	int i;
	for (i = 0 ; i < n ; i++)
		pc_mask[off + i] = 1;
	pc_maskrow[off / (int)vid.rowbytes] = 1;
	pc_maskrow[(off + n - 1) / (int)vid.rowbytes] = 1;
	if (off < pc_mask_lo) pc_mask_lo = off;
	if (off + n - 1 > pc_mask_hi) pc_mask_hi = off + n - 1;
}
extern int pc_2d_line;
#define PC_2D()			do { if (pc_pipe_on) { pc_2d_line = __LINE__; PC_PipeBefore2D (); } } while (0)
#define PC_2DPX(p)		do { if (pc_mask) PC_Mask1 ((int)((byte *)(p) - vid.conbuffer)); } while (0)
#define PC_2DROW(p, n)	do { if (pc_mask) PC_MaskN ((int)((byte *)(p) - vid.conbuffer), (n)); } while (0)

void	PC_PipeInit (byte *F, short *Fz, byte *w0, short *z0, byte *w1, short *z1, int w, int h);
void	PC_PipeFrameBegin (void);	// SCR_UpdateScreen, after its early returns
void	PC_PipeViewBegin (void);	// R_RenderView, before R_RenderView_
void	PC_PipeWorldDone (void);	// end of R_EdgeDrawing (replaces pcdsp_frame_end)
void	PC_PipeViewEnd (void);		// R_RenderView, after R_RenderView_
void	PC_PipeDisable (void);	// phase 9: the DSP is lost, the rest on the CPU
void	PC_PipeDrain (void);		// finish every pending frame (FadeScreen, screenshot, exit, end of timedemo)
int		PC_PipeCurrentPending (void);
void	PC_PipeRequestDump (int index);	// the harness: dump the current (pending) frame when it is finished
void	PC_PipeStats (void);		// PCPIPE line

#endif
