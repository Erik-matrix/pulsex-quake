/*
Copyright (C) 2026 the PulseX authors. Part of the PulseX Quake port, GPL v2 or later (see LICENSE).

sys_pc.c -- PulseX platform layer: files, time, errors and the main loop.

Replaces id's sys_win.c (which also carried the x86 FPU control-word code, the dedicated-server
console and the MGL/DirectDraw startup). Written against id's sys.h interface; the file-handle
table follows the shape of id's sys_null.c.
*/

#include "quakedef.h"
#include "pc_harness.h"
#include "pc_dsp.h"	// PulseX phase 2: DSP span drawing (bridge/pxquake)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <direct.h>
#include <errno.h>
#include <io.h>
#include <sys/stat.h>

qboolean	isDedicated;		// host.c asks; this build has no dedicated server mode

/*
===============================================================================

FILE IO

===============================================================================
*/

#define	MAX_HANDLES		64		// id's sys_win.c has 10; pak files stay open for the whole run
static FILE	*sys_handles[MAX_HANDLES];

static int findhandle (void)
{
	int		i;

	for (i = 1 ; i < MAX_HANDLES ; i++)
		if (!sys_handles[i])
			return i;
	Sys_Error ("out of handles");
	return -1;
}

static int pc_filelength (FILE *f)
{
	long long	pos, end;

	pos = _ftelli64 (f);
	_fseeki64 (f, 0, SEEK_END);
	end = _ftelli64 (f);
	_fseeki64 (f, pos, SEEK_SET);
	if (end > 0x7fffffff)
		Sys_Error ("file larger than 2 GB");	// Quake's file API is 32-bit
	return (int)end;
}

/*
 * Harness runs read default.cfg only. config.cfg and autoexec.cfg hold whatever the last
 * interactive session left (viewsize, gamma, fov, binds); a reference dump must not depend on
 * them. Interactive runs read both as usual.
 */
static int harness_hides (const char *path)
{
	const char	*base = path;
	const char	*p;

	if (!pc_harness)
		return 0;
	for (p = path ; *p ; p++)
		if (*p == '/' || *p == '\\')
			base = p + 1;
	return !Q_strcasecmp ((char *)base, "config.cfg") || !Q_strcasecmp ((char *)base, "autoexec.cfg");
}

int Sys_FileOpenRead (char *path, int *hndl)
{
	FILE	*f;
	int		i;

	if (harness_hides (path))
	{
		*hndl = -1;
		return -1;
	}
	i = findhandle ();
	f = fopen (path, "rb");
	if (!f)
	{
		*hndl = -1;
		return -1;
	}
	sys_handles[i] = f;
	*hndl = i;
	return pc_filelength (f);
}

int Sys_FileOpenWrite (char *path)
{
	FILE	*f;
	int		i;

	i = findhandle ();
	f = fopen (path, "wb");
	if (!f)
		Sys_Error ("Error opening %s: %s", path, strerror (errno));
	sys_handles[i] = f;
	return i;
}

void Sys_FileClose (int handle)
{
	if (handle <= 0 || handle >= MAX_HANDLES || !sys_handles[handle])
		return;
	fclose (sys_handles[handle]);
	sys_handles[handle] = NULL;
}

void Sys_FileSeek (int handle, int position)
{
	fseek (sys_handles[handle], position, SEEK_SET);
}

int Sys_FileRead (int handle, void *dest, int count)
{
	return (int)fread (dest, 1, count, sys_handles[handle]);
}

int Sys_FileWrite (int handle, void *data, int count)
{
	return (int)fwrite (data, 1, count, sys_handles[handle]);
}

int Sys_FileTime (char *path)
{
	struct _stat64	st;

	if (harness_hides (path))
		return -1;
	if (_stat64 (path, &st) != 0)
		return -1;
	return (int)st.st_mtime;
}

void Sys_mkdir (char *path)
{
	_mkdir (path);
}

/*
===============================================================================

SYSTEM IO

===============================================================================
*/

void Sys_MakeCodeWriteable (unsigned long startaddr, unsigned long length)
{
	// only the x86 assembly patched itself; id386 is 0 here
}

void Sys_DebugLog (char *file, char *fmt, ...)
{
	va_list		argptr;
	FILE		*f;

	f = fopen (file, "a");
	if (!f)
		return;
	va_start (argptr, fmt);
	vfprintf (f, fmt, argptr);
	va_end (argptr);
	fclose (f);
}

void Sys_Error (char *error, ...)
{
	va_list		argptr;
	char		text[2048];
	static int	in_error;

	va_start (argptr, error);
	vsnprintf (text, sizeof(text), error, argptr);
	va_end (argptr);

	fprintf (stderr, "Sys_Error: %s\n", text);
	fprintf (stdout, "Sys_Error: %s\n", text);
	fflush (stdout);
	fflush (stderr);

	if (!in_error)
	{
		in_error = 1;
		IN_Shutdown ();				// give the cursor back before anything else
		if (!pc_headless && !pc_harness)
			MessageBoxA (NULL, text, "PulseX Quake - error", MB_OK | MB_ICONERROR);
		VID_Shutdown ();
	}
	ExitProcess (1);
}

void Sys_Printf (char *fmt, ...)
{
	va_list		argptr;

	va_start (argptr, fmt);
	vfprintf (stdout, fmt, argptr);
	va_end (argptr);
	fflush (stdout);
}

void Sys_Quit (void)
{
	int		t, rc;

	t = COM_CheckParm ("-pctest_quitstall");	// TEST, known-sick for the close test: the shutdown stalls (no DSP involved)
	if (t && !pcdsp_lpm_ok ())
	{
		printf ("PCQUIT TEST REFUSED: the DSP did not apply -dsplpm/-dsplat (see 'PCDSP lpm'), so a stall would prove nothing - no stall\n");
		fflush (stdout);
		t = 0;
	}
	if (t && t < com_argc-1)
	{
		printf ("PCQUIT TEST: stalling %d ms before the shutdown\n", Q_atoi (com_argv[t+1]));
		fflush (stdout);
		Sleep (Q_atoi (com_argv[t+1]));
	}
	t = COM_CheckParm ("-pctest_quitstall_busy");	// TEST: the same stall with a busy host core (system idle vs DSP idle)
	if (t && !pcdsp_lpm_ok ())
		t = 0;	// refused as above
	if (t && t < com_argc-1)
	{
		double	e = PC_Seconds () + Q_atoi (com_argv[t+1]) / 1000.0;
		printf ("PCQUIT TEST: busy-stalling %d ms before the shutdown\n", Q_atoi (com_argv[t+1]));
		fflush (stdout);
		while (PC_Seconds () < e)
			;
	}
	printf ("PCQUIT begin t=%.3f s\n", PC_Seconds ());
	fflush (stdout);
	Host_Shutdown ();
	rc = pcdsp_exit_code ();	// PulseX phase 7: 0, or 7/8/9 = a DSP thread did not end cleanly / a bounded wait expired / an RPC in a window message
	printf ("PCQUIT done t=%.3f s exit=%d\n", PC_Seconds (), rc);
	fflush (stdout);
	fflush (stderr);
	ExitProcess (rc);		// as the harness leaves: the DSP session is closed already
}

static LARGE_INTEGER	qpc_freq, qpc_base;

double PC_Seconds (void)
{
	LARGE_INTEGER	now;

	QueryPerformanceCounter (&now);
	return (double)(now.QuadPart - qpc_base.QuadPart) / (double)qpc_freq.QuadPart;
}

double Sys_FloatTime (void)
{
	return PC_Seconds ();
}

char *Sys_ConsoleInput (void)
{
	return NULL;
}

void Sys_Sleep (void)
{
	Sleep (1);
}

void Sys_SendKeyEvents (void)
{
	VID_PC_PumpMessages ();
}

// x87 control-word switching existed only for the x86 assembly
void Sys_LowFPPrecision (void) {}
void Sys_HighFPPrecision (void) {}
void Sys_SetFPCW (void) {}

/*
===============================================================================

MAIN

===============================================================================
*/

static void attach_console (void)
{
	// GUI-subsystem exe: stdout works when the parent passed pipes/files (scripts);
	// started from a terminal, borrow the terminal's console.
	HANDLE	h = GetStdHandle (STD_OUTPUT_HANDLE);

	if (h != NULL && h != INVALID_HANDLE_VALUE && GetFileType (h) != FILE_TYPE_UNKNOWN)
		return;
	if (AttachConsole (ATTACH_PARENT_PROCESS))
	{
		freopen ("CONOUT$", "w", stdout);
		freopen ("CONOUT$", "w", stderr);
		return;
	}
	{	// PulseX phase 7: no stdout at all (the desktop shortcut): bin\pulsex-quake.log, the previous one kept as .old.log
		char	path[MAX_PATH], old[MAX_PATH];
		DWORD	n = GetModuleFileNameA (NULL, path, sizeof(path));
		char	*s;

		if (n == 0 || n >= sizeof(path) - 32)
			return;
		s = strrchr (path, '\\');
		if (!s)
			return;
		strcpy (s + 1, "pulsex-quake.log");
		strcpy (old, path);
		strcpy (old + strlen (old) - 4, ".old.log");
		MoveFileExA (path, old, MOVEFILE_REPLACE_EXISTING);
		if (freopen (path, "w", stdout))
			setvbuf (stdout, NULL, _IONBF, 0);
		if (freopen (path, "a", stderr))
			setvbuf (stderr, NULL, _IONBF, 0);
	}
}

double PC_HostTimeToNextFrame (void);	// PulseX phase 10 (host.c)
int main (int argc, char **argv)
{
	quakeparms_t	parms;
	static char		cwd[MAX_OSPATH];
	double			oldtime, newtime, time;
	int				t, lastframe;

	QueryPerformanceFrequency (&qpc_freq);
	QueryPerformanceCounter (&qpc_base);
	timeBeginPeriod (1);	// Sleep(1) below = 1 ms, not 15.6 ms
	attach_console ();

	memset (&parms, 0, sizeof(parms));
	COM_InitArgv (argc, argv);
	parms.argc = com_argc;
	parms.argv = com_argv;

	if (!_getcwd (cwd, sizeof(cwd)))
		strcpy (cwd, ".");
	if (cwd[strlen(cwd)-1] == '\\' || cwd[strlen(cwd)-1] == '/')
		cwd[strlen(cwd)-1] = 0;
	parms.basedir = cwd;	// -basedir overrides it in COM_InitFilesystem

	parms.memsize = 64*1024*1024;	// id's default is 8 MB (16 for GL); the hunk is one block
	t = COM_CheckParm ("-mem");
	if (t && t < com_argc-1)
		parms.memsize = (int)(Q_atof (com_argv[t+1]) * 1024 * 1024);
	parms.membase = malloc (parms.memsize);
	if (!parms.membase)
		Sys_Error ("Not enough memory free; check disk space\n");

	// PulseX phase 2: -dspbench <n> [w h] = FastRPC call-cost measurement only, then exit (no game)
	t = COM_CheckParm ("-dspbench");
	if (t && t < com_argc-1)
	{
		int bw = 640, bh = 480;
		if (t < com_argc-3) { bw = Q_atoi (com_argv[t+2]); bh = Q_atoi (com_argv[t+3]); }
		{
			int qs = COM_CheckParm ("-dspqspin"), qp = COM_CheckParm ("-dspqpoll");	// PulseX phase 5
			pcdsp_set_mb_timing (qs && qs < com_argc-1 ? Q_atoi (com_argv[qs+1]) : -1, qp && qp < com_argc-1 ? Q_atoi (com_argv[qp+1]) : 0);
		}
		if (COM_CheckParm ("-dspqbench"))	// PulseX phase 5: through the mailbox
			t = pcdsp_dqbench (Q_atoi (com_argv[t+1]), bw, bh);
		else
		t = pcdsp_bench (Q_atoi (com_argv[t+1]), bw, bh);
		fflush (stdout);
		exit (t);
	}

	PC_HarnessInit ();

	printf ("PulseX Quake (id Software WinQuake %4.2f, software renderer, ARM64)\n", (float)VERSION);
	Host_Init (&parms);
	PC_HarnessStart ();

	oldtime = Sys_FloatTime ();
	while (1)
	{
		newtime = Sys_FloatTime ();
		time = newtime - oldtime;
		oldtime = newtime;
		if (pc_fixeddt > 0)
			time = pc_fixeddt;

		lastframe = host_framecount;
		PC_HarnessFrameBegin ();
		Host_Frame ((float)time);
		PC_HarnessFrameEnd ();

		// Host_FilterTime holds frames to 72 Hz outside timedemo; do not spin a core meanwhile
		if (host_framecount == lastframe && !cls.timedemo && PC_HostTimeToNextFrame () > 0)	// PulseX phase 10: wait for the deadline
		{
			static HANDLE	hrt;
			static int		hrt_tried;
			const double	rem = PC_HostTimeToNextFrame ();
			if (!hrt_tried)
			{
				hrt_tried = 1;
				hrt = CreateWaitableTimerExW (NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
			}
			if (rem > 0.0005 && hrt)
			{
				LARGE_INTEGER	due;
				due.QuadPart = -(LONGLONG)((rem - 0.0003) * 1e7);	// relative, 100 ns units
				if (SetWaitableTimer (hrt, &due, 0, NULL, NULL, FALSE))
					WaitForSingleObject (hrt, 20);
				else
					Sleep (1);
			}
			else if (rem > 0.0015)
				Sleep (1);
			else
				SwitchToThread ();
		}
		else
		if (host_framecount == lastframe && !cls.timedemo)
			Sleep (1);
	}
	return 0;
}
