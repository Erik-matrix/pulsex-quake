/*
Copyright (C) 2026 the PulseX authors. Part of the PulseX Quake port, GPL v2 or later (see LICENSE).

pc_harness.c -- PulseX test harness: timedemo, frame dumps and the per-stage profiler.

  -timedemo <demo>        play <demo> as fast as possible, print the result, exit.
                          Headless (no window, no D3D11) unless -window is given.
  -framedump <dir> <N>    during the timedemo, write every Nth frame:
                            <dir>\fNNNNNN.raw  width*height bytes, the 8-bit framebuffer
                            <dir>\fNNNNNN.pal  768 bytes, the palette in effect (incl. flashes)
                            <dir>\fNNNNNN.png  an indexed PNG of every -pngevery'th dump (8)
                            <dir>\dump.txt     the run's parameters
                          Implies -fixeddt 0.0138888 (1/72 s) unless -fixeddt is given.
  -fixeddt <seconds>      every Host_Frame gets exactly this time step instead of the wall-clock
                          delta. This is what makes a dump reproducible: in a timedemo the demo
                          advances one message per frame, but cl.time (interpolation, animation,
                          particles, the console slide, palette flashes) advances by the frame
                          time, which with a real clock depends on how fast the machine was.
  -profile                exclusive per-stage timers (pc_prof.h), printed at the end over the
                          same Host_Frame calls the fps figure covers.
  -selftest <dir>         start e1m1 with a local server (QuakeC runs), check the 64-bit string
                          fixes in the running game, write <dir>\selftest.png, exit 0 if all ok.
  -quitafter <seconds>    leave after that many seconds (smoke tests of the interactive path).

In every harness mode (-timedemo, -selftest) config.cfg and autoexec.cfg are not read (sys_pc.c),
the console starts empty (no build timestamp in the frames), the mouse is never captured, the
window (with -window) does not take the focus, and the run exits without Host_Shutdown, so it
never writes config.cfg either.

Output lines meant for scripts start with "PCRESULT" / "PCPROF" / "PCDUMP".
*/

#include "quakedef.h"
#include "pc_harness.h"
#include "pc_pipe.h"	// PulseX phase 4

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>

int		pc_harness;
int		pc_headless;
double	pc_fixeddt;

static char		td_demo[MAX_QPATH];
static char		dump_dir[MAX_OSPATH];
static int		dump_every;
static int		dump_pngevery = 8;
static int		dump_count;
static int		turb_frames, turb_frames_water, turb_dumped, turb_dumped_water;	// PulseX phase 6: water coverage
static double	turb_pix_sum, turb_dsp_sum;
static char		turb_list[2048];		// dump indices with water
static char		turb_dsp_list[2048];	// dump indices whose water went to the DSP
static int		turb_dumped_dsp;
extern int		pc_turb_pix_frame, pc_turb_dsp_frame;	// pc/d_dspspans.c
static int		td_seen;			// cls.timedemo was true at some point
static int		td_was;
static int		td_loops;			// PulseX phase 9: -pctdloop N, N more runs of the timedemo in the same session
static int		frame_no;			// host_framecount at the start of this frame
static double	frame_t0;
static double	wall_t0;
static int		wall_t0_set;
static double	dump_seconds;

/*
===============================================================================

PROFILER

===============================================================================
*/

int pc_prof_on;

const char *pc_stage_name[PS_COUNT] =
{
	"host", "screen2d", "rview", "bsp", "edges", "dsurf", "surfcache", "surfbuild",
	"spans", "zspans", "turb", "sky", "alias", "sprite", "particle", "warp",
	"palconv", "present", "dump"
};

#define PROF_STACK	32

static long long	prof_freq;
static long long	prof_t0;
static int			prof_cur;
static int			prof_sp;
static int			prof_stack[PROF_STACK];
static int			prof_bad;					// push/pop mismatch seen
static long long	frame_acc[PS_COUNT];
static int			frame_calls[PS_COUNT];
static long long	total_acc[PS_COUNT];
static long long	total_calls[PS_COUNT];
static int			total_frames;
static int			first_frame = -1, last_frame = -1;	// host_framecount of the first / last committed frame
static float		*frame_ms;					// per measured frame, for median / p99
static int			frame_ms_max;

static inline long long qpc (void)
{
	LARGE_INTEGER	t;

	QueryPerformanceCounter (&t);
	return t.QuadPart;
}

void PC_ProfPush (int stage)
{
	long long	now = qpc ();

	frame_acc[prof_cur] += now - prof_t0;
	if (prof_sp >= PROF_STACK)
	{
		prof_bad = 1;
		return;
	}
	prof_stack[prof_sp++] = prof_cur;
	prof_cur = stage;
	frame_calls[stage]++;
	prof_t0 = now;
}

void PC_ProfPop (void)
{
	long long	now = qpc ();

	frame_acc[prof_cur] += now - prof_t0;
	if (prof_sp <= 0)
	{
		prof_bad = 1;
		return;
	}
	prof_cur = prof_stack[--prof_sp];
	prof_t0 = now;
}

static void prof_frame_begin (void)
{
	memset (frame_acc, 0, sizeof(frame_acc));
	memset (frame_calls, 0, sizeof(frame_calls));
	prof_cur = PS_HOST;
	prof_sp = 0;
	prof_t0 = qpc ();
}

static void prof_frame_end (int commit)
{
	long long	sum = 0;
	int			i;

	frame_acc[prof_cur] += qpc () - prof_t0;
	if (prof_sp != 0 || prof_cur != PS_HOST)
		prof_bad = 1;		// a stage was left open: the numbers would be wrong, say so at the end
	if (!commit)
		return;
	if (first_frame < 0)
		first_frame = frame_no;
	last_frame = frame_no;
	for (i = 0 ; i < PS_COUNT ; i++)
	{
		total_acc[i] += frame_acc[i];
		total_calls[i] += frame_calls[i];
		sum += frame_acc[i];
	}
	if (total_frames >= frame_ms_max)
	{
		frame_ms_max = frame_ms_max ? frame_ms_max * 2 : 4096;
		frame_ms = realloc (frame_ms, frame_ms_max * sizeof(float));
	}
	frame_ms[total_frames++] = (float)(sum * 1000.0 / prof_freq);
}

static int cmp_float (const void *a, const void *b)
{
	float	x = *(const float *)a, y = *(const float *)b;

	return x < y ? -1 : x > y;
}

static void prof_report (void)
{
	long long	sum = 0;
	int			i;
	double		ms;

	if (!total_frames)
	{
		printf ("PCPROF no frames measured\n");
		return;
	}
	for (i = 0 ; i < PS_COUNT ; i++)
		sum += total_acc[i];
	qsort (frame_ms, total_frames, sizeof(float), cmp_float);
	printf ("PCPROF frames=%d first=%d last=%d td_startframe=%d qpc_hz=%lld total_ms_per_frame=%.4f median_ms=%.4f p99_ms=%.4f max_ms=%.4f stack_ok=%d\n",
		total_frames, first_frame, last_frame, cls.td_startframe, prof_freq, sum * 1000.0 / prof_freq / total_frames,
		frame_ms[total_frames / 2], frame_ms[(int)(total_frames * 0.99)], frame_ms[total_frames - 1],
		!prof_bad);
	for (i = 0 ; i < PS_COUNT ; i++)
	{
		if (i == PS_DUMP)
			continue;
		ms = total_acc[i] * 1000.0 / prof_freq / total_frames;
		printf ("PCPROF stage=%-10s ms_per_frame=%9.4f pct=%6.2f calls_per_frame=%9.1f\n",
			pc_stage_name[i], ms, sum ? 100.0 * total_acc[i] / sum : 0.0,
			(double)total_calls[i] / total_frames);
	}
	if (prof_bad)
		printf ("PCPROF WARNING: push/pop mismatch seen, the stage split is NOT trustworthy\n");
}

/*
===============================================================================

FRAME DUMP

===============================================================================
*/

static unsigned	crc_table[256];

static void crc_init (void)
{
	unsigned	c;
	int			n, k;

	for (n = 0 ; n < 256 ; n++)
	{
		c = (unsigned)n;
		for (k = 0 ; k < 8 ; k++)
			c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}

static unsigned crc_update (unsigned crc, const byte *p, size_t n)
{
	while (n--)
		crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return crc;
}

static void put_be32 (byte *p, unsigned v)
{
	p[0] = (byte)(v >> 24); p[1] = (byte)(v >> 16); p[2] = (byte)(v >> 8); p[3] = (byte)v;
}

static void png_chunk (FILE *f, const char *type, const byte *data, unsigned len)
{
	byte		hdr[8];
	unsigned	crc;

	put_be32 (hdr, len);
	memcpy (hdr + 4, type, 4);
	fwrite (hdr, 1, 8, f);
	if (len)
		fwrite (data, 1, len, f);
	crc = crc_update (0xffffffffu, (const byte *)type, 4);
	crc = crc_update (crc, data, len) ^ 0xffffffffu;
	put_be32 (hdr, crc);
	fwrite (hdr, 1, 4, f);
}

// indexed-colour PNG (the exact 8-bit indices + PLTE), zlib "stored" blocks: no compressor needed
static void write_png (const char *path, const byte *pix, int w, int h, int stride, const byte *pal)
{
	static const byte	sig[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
	byte				ihdr[13];
	byte				*raw, *z, *zp;
	size_t				rawlen, zlen, left, blk;
	unsigned			a = 1, b = 0;
	size_t				i;
	int					y;
	FILE				*f;

	rawlen = (size_t)h * (w + 1);
	raw = malloc (rawlen);
	for (y = 0 ; y < h ; y++)
	{
		raw[(size_t)y * (w + 1)] = 0;		// filter: none
		memcpy (raw + (size_t)y * (w + 1) + 1, pix + (size_t)y * stride, w);
	}
	zlen = 2 + rawlen + 5 * (rawlen / 65535 + 1) + 4;
	z = malloc (zlen);
	zp = z;
	*zp++ = 0x78; *zp++ = 0x01;
	for (left = rawlen, i = 0 ; ; )
	{
		blk = left > 65535 ? 65535 : left;
		*zp++ = (left == blk);
		*zp++ = (byte)blk; *zp++ = (byte)(blk >> 8);
		*zp++ = (byte)~blk; *zp++ = (byte)(~blk >> 8);
		memcpy (zp, raw + i, blk);
		zp += blk; i += blk; left -= blk;
		if (!left)
			break;
	}
	for (i = 0 ; i < rawlen ; i++)
	{
		a = (a + raw[i]) % 65521;
		b = (b + a) % 65521;
	}
	put_be32 (zp, (b << 16) | a);
	zp += 4;

	f = fopen (path, "wb");
	if (f)
	{
		fwrite (sig, 1, 8, f);
		put_be32 (ihdr, w);
		put_be32 (ihdr + 4, h);
		ihdr[8] = 8; ihdr[9] = 3; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
		png_chunk (f, "IHDR", ihdr, 13);
		png_chunk (f, "PLTE", pal, 768);
		png_chunk (f, "IDAT", z, (unsigned)(zp - z));
		png_chunk (f, "IEND", NULL, 0);
		fclose (f);
	}
	free (z);
	free (raw);
}

static void write_file (const char *path, const void *data, size_t len)
{
	FILE	*f = fopen (path, "wb");

	if (!f)
		Sys_Error ("framedump: cannot write %s", path);
	if (fwrite (data, 1, len, f) != len)
		Sys_Error ("framedump: short write on %s (disk full?)", path);
	fclose (f);
}

static void dump_from (int index, const byte *buf, const unsigned char *pal);
static void dump_frame (int index)
{
	if (PC_PipeCurrentPending ())	// PulseX phase 4: written when the pipeline finishes the frame
	{
		PC_PipeRequestDump (index);
		return;
	}
	dump_from (index, vid.buffer, pc_palette);
}
void PC_HarnessDumpFrom (int index, const byte *buf, const unsigned char *pal)
{
	dump_from (index, buf, pal);
}
static void dump_from (int index, const byte *buf, const unsigned char *pal)
{
	char	path[MAX_OSPATH + 32];
	byte	*rows;
	unsigned y;
	double	t = PC_Seconds ();

	// vid.rowbytes == vid.width in vid_pc.c, but do not rely on it
	rows = malloc ((size_t)vid.width * vid.height);
	for (y = 0 ; y < vid.height ; y++)
		memcpy (rows + (size_t)y * vid.width, buf + (size_t)y * vid.rowbytes, vid.width);

	snprintf (path, sizeof(path), "%s\\f%06d.raw", dump_dir, index);
	write_file (path, rows, (size_t)vid.width * vid.height);
	snprintf (path, sizeof(path), "%s\\f%06d.pal", dump_dir, index);
	write_file (path, pal, 768);
	if (dump_pngevery > 0 && (dump_count % dump_pngevery) == 0)
	{
		snprintf (path, sizeof(path), "%s\\f%06d.png", dump_dir, index);
		write_png (path, rows, vid.width, vid.height, vid.width, (unsigned char *)pal);
	}
	free (rows);
	dump_count++;
	dump_seconds += PC_Seconds () - t;
}

/*
===============================================================================

SELFTEST (-selftest <dir>)

Loads e1m1 with a local server, lets it run 72 frames, then checks the 64-bit QuakeC string
fixes inside the running game and writes the frame as <dir>\selftest.png. Each check prints
PCSELFTEST name=... ok=0/1. The "control" checks show that the fault the fix guards against
is really present in this process (static storage out of int reach of pr_strings); if a control
says 0 the corresponding fix is untested here, not proven.
===============================================================================
*/

static char		selftest_dir[MAX_OSPATH];
static int		selftest_frames;
static double	quitafter;
static double	quit_t0;
static char		selftest_static[128] = "static";	// lives in .data, like id's pr_string_temp[128] did

void PF_ftos (void);
void PF_vtos (void);

static int st_check (const char *name, int ok, const char *detail)
{
	printf ("PCSELFTEST name=%s ok=%d %s\n", name, ok, detail ? detail : "");
	return ok;
}

static void selftest_frame (void)
{
	char		path[MAX_OSPATH + 32];
	char		detail[256];
	ptrdiff_t	d;
	int			all = 1;

	if (!sv.active || cls.signon != SIGNONS)
		return;
	if (++selftest_frames < 72)
		return;

	// the control: is static storage out of reach of an int offset from pr_strings in this process?
	d = selftest_static - pr_strings;
	snprintf (detail, sizeof(detail), "static-pr_strings=%lld (fits int: %d) -> the old \"ptr - pr_strings\" truncates",
		(long long)d, d == (int)d);
	st_check ("control_static_out_of_reach", d != (int)d, detail);

	// sv.name (static) as QuakeC's mapname
	snprintf (detail, sizeof(detail), "mapname=\"%s\" sv.name=\"%s\"", pr_strings + pr_global_struct->mapname, sv.name);
	all &= st_check ("mapname", !strcmp (pr_strings + pr_global_struct->mapname, sv.name), detail);

	// mod_known[].name (static) as world.model
	snprintf (detail, sizeof(detail), "world.model=\"%s\" worldmodel=\"%s\"", pr_strings + sv.edicts->v.model, sv.worldmodel->name);
	all &= st_check ("world_model", !strcmp (pr_strings + sv.edicts->v.model, sv.worldmodel->name), detail);

	// the temp-string builtins
	G_FLOAT(OFS_PARM0) = 42;
	PF_ftos ();
	snprintf (detail, sizeof(detail), "ftos(42)=\"%s\"", G_STRING(OFS_RETURN));
	all &= st_check ("ftos", !strcmp (G_STRING(OFS_RETURN), "42"), detail);
	G_FLOAT(OFS_PARM0) = 1; G_FLOAT(OFS_PARM0+1) = -2.5; G_FLOAT(OFS_PARM0+2) = 3;
	PF_vtos ();
	snprintf (detail, sizeof(detail), "vtos('1 -2.5 3')=\"%s\"", G_STRING(OFS_RETURN));
	all &= st_check ("vtos", !strcmp (G_STRING(OFS_RETURN), "'  1.0  -2.5   3.0'"), detail);

	// the player's netname is the client name (hunk)
	snprintf (detail, sizeof(detail), "netname=\"%s\" client=\"%s\"", pr_strings + svs.clients[0].edict->v.netname, svs.clients[0].name);
	all &= st_check ("netname", !strcmp (pr_strings + svs.clients[0].edict->v.netname, svs.clients[0].name), detail);

	snprintf (path, sizeof(path), "%s\\selftest.png", selftest_dir);
	write_png (path, vid.buffer, vid.width, vid.height, vid.rowbytes, pc_palette);
	printf ("PCSELFTEST all_ok=%d frames=%d png=%s\n", all, host_framecount, path);
	PC_HarnessExit (all ? 0 : 4);
}

/*
===============================================================================

HARNESS

===============================================================================
*/

void PC_HarnessInit (void)
{
	LARGE_INTEGER	f;
	int				t;

	QueryPerformanceFrequency (&f);
	prof_freq = f.QuadPart;
	crc_init ();

	t = COM_CheckParm ("-pctdloop");
	if (t && t < com_argc-1)
		td_loops = Q_atoi (com_argv[t+1]);
	t = COM_CheckParm ("-timedemo");
	if (t && t < com_argc-1)
	{
		Q_strncpy (td_demo, com_argv[t+1], sizeof(td_demo) - 1);
		pc_harness = 1;
		pc_headless = 1;
	}
	t = COM_CheckParm ("-framedump");
	if (t)
	{
		if (t >= com_argc-2 || !td_demo[0])
			Sys_Error ("usage: -timedemo <demo> -framedump <dir> <every N frames>");
		Q_strncpy (dump_dir, com_argv[t+1], sizeof(dump_dir) - 1);
		dump_every = Q_atoi (com_argv[t+2]);
		if (dump_every < 1)
			dump_every = 1;
		pc_fixeddt = 1.0 / 72.0;
		_mkdir (dump_dir);
	}
	t = COM_CheckParm ("-pngevery");
	if (t && t < com_argc-1)
		dump_pngevery = Q_atoi (com_argv[t+1]);
	t = COM_CheckParm ("-fixeddt");
	if (t && t < com_argc-1)
		{	// PulseX phase 5: a/b is a fraction ("1/72"); Q_atof alone read it as 1.0
			char	*arg = com_argv[t+1], *sl = strchr (arg, '/');
			pc_fixeddt = Q_atof (arg);
			if (sl && Q_atof (sl + 1) > 0)
				pc_fixeddt = Q_atof (arg) / Q_atof (sl + 1);
		}
	if (COM_CheckParm ("-headless"))
		pc_headless = 1;
	if (COM_CheckParm ("-window"))
		pc_headless = 0;
	if (COM_CheckParm ("-profile"))
		pc_prof_on = 1;
	t = COM_CheckParm ("-selftest");
	if (t && t < com_argc-1)
	{
		Q_strncpy (selftest_dir, com_argv[t+1], sizeof(selftest_dir) - 1);
		_mkdir (selftest_dir);
		pc_harness = 1;
		if (!COM_CheckParm ("-window"))
			pc_headless = 1;
	}
	t = COM_CheckParm ("-quitafter");
	if (t && t < com_argc-1)
		quitafter = Q_atof (com_argv[t+1]);
	quit_t0 = PC_Seconds ();
}

void PC_HarnessStart (void)
{
	FILE	*f;
	char	path[MAX_OSPATH + 32];

	if (pc_harness)
	{
		// Host_Init printed "Exe: <__TIME__> <__DATE__>" and the heap size into the console, and the
		// console is fully down in the first frames of a timedemo: a rebuild alone would change the
		// dumped pixels. Start harness runs with an empty console.
		Con_Clear_f ();
		Con_ClearNotify ();
	}
	if (selftest_dir[0])
		Cbuf_AddText ("map e1m1\n");	// a real server: QuakeC runs, unlike in a demo
	if (!td_demo[0])
		return;
	Cbuf_AddText (va ("timedemo %s\n", td_demo));
	if (dump_dir[0])
	{
		snprintf (path, sizeof(path), "%s\\dump.txt", dump_dir);
		f = fopen (path, "w");
		if (!f)
			Sys_Error ("framedump: cannot write %s", path);
		fprintf (f, "demo %s\nwidth %u\nheight %u\nevery %d\nfixeddt %.9f\nformat fNNNNNN.raw = width*height 8-bit indices, rows top-down; fNNNNNN.pal = 768 bytes RGB\n",
			td_demo, vid.width, vid.height, dump_every, pc_fixeddt);
		fclose (f);
	}
}

void PC_HarnessFrameBegin (void)
{
	frame_no = host_framecount;
	frame_t0 = PC_Seconds ();
	if (cls.timedemo && host_framecount == cls.td_startframe + 1)
	{
		wall_t0 = frame_t0;		// the same frame CL_GetMessage takes td_starttime in
		wall_t0_set = 1;
	}
	if (pc_prof_on)
		prof_frame_begin ();
}

void PC_HarnessExit (int code)
{
	fflush (stdout);
	IN_Shutdown ();
	VID_Shutdown ();
	ExitProcess (code);
}

void PC_HarnessFrameEnd (void)
{
	int		in_td = cls.timedemo;
	int		index, frames;
	double	wall;

	// a frame counts if the timedemo is still running after it and it is past the start frame
	if (pc_prof_on)
		prof_frame_end (in_td && td_was && frame_no >= cls.td_startframe + 1);

	if (td_was || in_td)
	{
		index = frame_no - cls.td_startframe;
		if (dump_dir[0] && in_td && (index % dump_every) == 0)
			dump_frame (index);
		if (in_td)	// PulseX phase 6: which frames (and which dumps) contain water
		{
			int	dumped = dump_dir[0] && (index % dump_every) == 0;
			turb_frames++;
			turb_pix_sum += pc_turb_pix_frame;
			turb_dsp_sum += pc_turb_dsp_frame;
			if (pc_turb_pix_frame)
				turb_frames_water++;
			if (dumped)
			{
				turb_dumped++;
				if (pc_turb_pix_frame)
				{
					size_t	l = strlen (turb_list);
					turb_dumped_water++;
					if (l + 12 < sizeof(turb_list))
						snprintf (turb_list + l, sizeof(turb_list) - l, "%s%d", l ? "," : "", index);
				}
				if (pc_turb_dsp_frame)
				{
					size_t	l = strlen (turb_dsp_list);
					turb_dumped_dsp++;
					if (l + 12 < sizeof(turb_dsp_list))
						snprintf (turb_dsp_list + l, sizeof(turb_dsp_list) - l, "%s%d", l ? "," : "", index);
				}
			}
		}
	}
	pc_turb_pix_frame = 0;
	pc_turb_dsp_frame = 0;
	if (in_td)
		td_seen = 1;

	if (td_was && !in_td)
	{
		// CL_FinishTimeDemo ran in this frame and printed Quake's own line
		frames = frame_no - cls.td_startframe - 1;
		wall = frame_t0 - wall_t0;
		if (pc_pipe_on)	// PulseX phase 4: the last pending frame is finished, and that time counts
		{
			double	td0 = PC_Seconds ();
			PC_PipeDrain ();
			wall += PC_Seconds () - td0;
		}
		printf ("PCRESULT demo=%s frames=%d wall_s=%.4f fps=%.2f width=%u height=%u headless=%d fixeddt=%.6f profile=%d dumps=%d dump_s=%.3f\n",
			td_demo, frames, wall, (wall_t0_set && wall > 0) ? frames / wall : 0.0,
			vid.width, vid.height, pc_headless, pc_fixeddt, pc_prof_on, dump_count, dump_seconds);
		if (dump_dir[0])
			printf ("PCDUMP dir=%s files=%d every=%d\n", dump_dir, dump_count, dump_every);
		printf ("PCTURB frames=%d frames_with_water=%d water_pix_per_frame=%.0f dsp_water_pix_per_frame=%.0f dumps=%d dumps_with_water=%d dump_idx_with_water=%s dumps_with_dsp_water=%d dump_idx_with_dsp_water=%s\n",
			turb_frames, turb_frames_water, turb_frames ? turb_pix_sum / turb_frames : 0.0, turb_frames ? turb_dsp_sum / turb_frames : 0.0,
			turb_dumped, turb_dumped_water, turb_list[0] ? turb_list : "-", turb_dumped_dsp, turb_dsp_list[0] ? turb_dsp_list : "-");
		if (pc_prof_on)
			prof_report ();
		if (td_loops > 0)		// PulseX phase 9: the same demo again, the session (and the DSP) stays
		{
			td_loops--;
			wall_t0_set = 0;
			td_was = 0;
			Cbuf_AddText (va ("timedemo %s\n", td_demo));
			return;
		}
		PC_HarnessExit (0);
	}
	td_was = in_td;

	if (td_demo[0] && !td_seen && host_framecount > 2000)
	{
		printf ("PCRESULT demo=%s error=timedemo_never_started\n", td_demo);
		PC_HarnessExit (3);
	}

	if (selftest_dir[0])
		selftest_frame ();

	if (quitafter > 0 && PC_Seconds () - quit_t0 > quitafter)
	{
		printf ("PCQUIT after %.1f s, %d frames, map %s\n", PC_Seconds () - quit_t0, host_framecount,
			cl.worldmodel ? cl.worldmodel->name : "(none)");
		PC_HarnessExit (0);
	}
}
