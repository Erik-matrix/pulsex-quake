/*
Copyright (C) 2026 the PulseX authors. Part of the PulseX Quake port, GPL v2 or later (see LICENSE).

pc_harness.h -- PulseX: the test harness (-timedemo, -framedump, -profile, -fixeddt).
*/
#ifndef PC_HARNESS_H
#define PC_HARNESS_H

extern int		pc_harness;		// any harness mode active (-timedemo or -framedump)
extern int		pc_headless;	// no window, no D3D11
extern double	pc_fixeddt;		// > 0: every Host_Frame gets exactly this time step

void	PC_HarnessInit (void);			// after COM_InitArgv, before Host_Init
void	PC_HarnessStart (void);			// after Host_Init: queue the timedemo
void	PC_HarnessFrameBegin (void);	// before Host_Frame
void	PC_HarnessFrameEnd (void);		// after Host_Frame: dumps, end-of-demo, exit
void	PC_HarnessExit (int code);		// leave without Host_Shutdown (no config.cfg write)
double	PC_Seconds (void);				// QueryPerformanceCounter in seconds

// vid_pc.c
extern byte	pc_palette[768];			// the palette currently on screen (incl. flashes)
void	VID_PC_PumpMessages (void);
void	*VID_PC_Window (void);			// HWND, or NULL when headless

// in_pc.c
long long IN_PC_WndProc (void *hwnd, unsigned msg, unsigned long long wparam, long long lparam, int *handled);
void	IN_PC_Frame (void);				// capture / release the cursor
void	IN_PC_Activate (int active);

#endif
