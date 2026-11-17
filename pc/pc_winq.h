/*
Copyright (C) 2026 the PulseX authors. Part of the PulseX Quake port, GPL v2 or later (see LICENSE).

pc_winq.h -- PulseX: the two things menu.c takes from id's winquake.h.

id's winquake.h pulls in DirectDraw, DirectSound, MGL and WinSock declarations that this port
does not use. menu.c (under _WIN32) only needs the window mode and the windowed-mouse cvar,
so it includes this header instead (marked "// PulseX:" in menu.c).
*/
#ifndef PC_WINQ_H
#define PC_WINQ_H

typedef enum {MS_WINDOWED, MS_FULLSCREEN, MS_FULLDIB, MS_UNINIT} modestate_t;

extern modestate_t	modestate;		// vid_pc.c: always MS_WINDOWED
extern cvar_t		_windowed_mouse;	// in_pc.c: capture the mouse in the window

#endif
