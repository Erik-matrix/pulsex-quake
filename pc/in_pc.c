/*
Copyright (C) 2026 the PulseX authors. Part of the PulseX Quake port, GPL v2 or later (see LICENSE).

in_pc.c -- PulseX input: Win32 keyboard and mouse.

Replaces id's in_win.c (DirectInput / cursor re-centering / joystick). Keyboard keys are mapped
by SCAN CODE with id's own table from vid_win.c (so the layout is the US one id shipped: the key
left of 1 opens the console on any keyboard). The mouse is read as raw input (WM_INPUT, relative
counts, no acceleration) and turned into view angles the way in_win.c's IN_MouseMove did, with
mouse look on by default (cvar m_freelook 1).

The cursor is captured (hidden, clipped to the window) only while the window is active, the
game itself has the keyboard (key_dest == key_game: no menu, no console), no demo is playing,
the game is not paused and _windowed_mouse is 1. Esc opens the menu, which releases it; leaving
the menu captures it again. Losing focus releases it too.
*/

#include "quakedef.h"
#include "pc_harness.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

cvar_t	_windowed_mouse = {"_windowed_mouse", "1", true};
cvar_t	m_filter = {"m_filter", "0"};
cvar_t	m_freelook = {"m_freelook", "1", true};	// PulseX: mouse look without +mlook

static int		in_active;		// window has focus
static int		in_captured;
static int		in_rawreg;
static int		in_released_by_esc;
static int		mouse_dx, mouse_dy;
static int		w_raw_msgs, w_raw_mouse, w_accepted, w_move_calls, w_move_captured;	// PulseX: the mouse witness
static long long	w_dx_total, w_dy_total;
static double	w_yaw_applied;
static float	old_mouse_x, old_mouse_y;

// PulseX phase 7: the window procedure runs no game code. Key and button events (and "clear the key states" on focus
// loss, key -1) are queued here and handed to Key_Event by IN_PC_DrainEvents after the message loop - before this,
// 'Y' in the quit menu ran Host_Shutdown and the DSP shutdown inside DispatchMessage (the hang of 2026-10-27).
#define IN_QMAX 512
static int		in_qkey[IN_QMAX], in_qdown[IN_QMAX], in_qn;
static long	in_qdropped;
static void in_queue (int key, int down)
{
	if (in_qn >= IN_QMAX)
	{
		in_qdropped++;
		return;
	}
	in_qkey[in_qn] = key;
	in_qdown[in_qn] = down;
	in_qn++;
}
void IN_PC_DrainEvents (void)
{
	int		k[IN_QMAX], d[IN_QMAX], n, i;

	n = in_qn;		// a copy first: Key_Event can pump messages again (SCR_ModalMessage) and queue more
	for (i = 0 ; i < n ; i++)
	{
		k[i] = in_qkey[i];
		d[i] = in_qdown[i];
	}
	in_qn = 0;
	for (i = 0 ; i < n ; i++)
	{
		if (k[i] < 0)
			Key_ClearStates ();
		else
			Key_Event (k[i], d[i]);
	}
}

/*
id Software's scan code table, from WinQuake vid_win.c (GPL).
*/
static const byte scantokey[128] =
{
//  0           1       2       3       4       5       6       7
//  8           9       A       B       C       D       E       F
	0  ,    27,     '1',    '2',    '3',    '4',    '5',    '6',
	'7',    '8',    '9',    '0',    '-',    '=',    K_BACKSPACE, 9, // 0
	'q',    'w',    'e',    'r',    't',    'y',    'u',    'i',
	'o',    'p',    '[',    ']',    13 ,    K_CTRL,'a',  's',      // 1
	'd',    'f',    'g',    'h',    'j',    'k',    'l',    ';',
	'\'' ,    '`',    K_SHIFT,'\\',  'z',    'x',    'c',    'v',      // 2
	'b',    'n',    'm',    ',',    '.',    '/',    K_SHIFT,'*',
	K_ALT,' ',   0  ,    K_F1, K_F2, K_F3, K_F4, K_F5,   // 3
	K_F6, K_F7, K_F8, K_F9, K_F10,  K_PAUSE,    0  , K_HOME,
	K_UPARROW,K_PGUP,'-',K_LEFTARROW,'5',K_RIGHTARROW,'+',K_END, //4
	K_DOWNARROW,K_PGDN,K_INS,K_DEL,0,0,             0,              K_F11,
	K_F12,0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,        // 5
	0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,
	0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,        // 6
	0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,
	0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0         // 7
};

static int MapKey (LPARAM lp)
{
	int		key = (int)((lp >> 16) & 255);

	if (key > 127)
		return 0;
	return scantokey[key];
}

/*
================
cursor capture
================
*/
static void capture (int on)
{
	HWND	hwnd = (HWND)VID_PC_Window ();
	RECT	rc;
	POINT	tl, br;

	if (!hwnd || on == in_captured)
		return;
	in_captured = on;
	if (on)
	{
		GetClientRect (hwnd, &rc);
		tl.x = rc.left; tl.y = rc.top;
		br.x = rc.right; br.y = rc.bottom;
		ClientToScreen (hwnd, &tl);
		ClientToScreen (hwnd, &br);
		rc.left = tl.x; rc.top = tl.y; rc.right = br.x; rc.bottom = br.y;
		ClipCursor (&rc);
		SetCapture (hwnd);
		while (ShowCursor (FALSE) >= 0)
			;
	}
	else
	{
		ClipCursor (NULL);
		ReleaseCapture ();
		while (ShowCursor (TRUE) < 0)
			;
	}
	mouse_dx = mouse_dy = 0;
}

void IN_PC_Activate (int active)
{
	in_active = active;
	if (!active)
	{
		capture (0);
		in_queue (-1, 0);		// no keys stuck down while another window has focus (Key_ClearStates, after the message loop)
	}
}

static int	w_fg_changes;		// PulseX: how often the per-frame foreground check changed in_active
void IN_PC_Frame (void)
{
	int		want;
	HWND	hwnd = (HWND)VID_PC_Window ();

	if (hwnd && in_rawreg)		// PulseX: the foreground window decides, every frame (not one check in IN_Init + WM_ACTIVATE)
	{
		int	fg = GetForegroundWindow () == hwnd && !IsIconic (hwnd);
		if (fg != in_active)
		{
			if (w_fg_changes < 20)
				printf ("PCIN active %d->%d (foreground check)\n", in_active, fg);
			w_fg_changes++;
			IN_PC_Activate (fg);
		}
	}
	want = in_rawreg && in_active && _windowed_mouse.value && key_dest == key_game
		&& !cls.demoplayback && !cl.paused && !in_released_by_esc;
	if (key_dest != key_game)
		in_released_by_esc = 0;		// the menu/console took over; leaving it re-captures
	capture (want);
}

/*
================
window messages (called first by vid_pc.c's window procedure)
================
*/
void VID_PC_ToggleFullscreen (void);
long long IN_PC_WndProc (void *hwnd_, unsigned msg, unsigned long long wparam, long long lparam, int *handled)
{
	HWND	hwnd = (HWND)hwnd_;
	WPARAM	wp = (WPARAM)wparam;
	LPARAM	lp = (LPARAM)lparam;
	int		key;

	*handled = 0;
	switch (msg)
	{
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (msg == WM_SYSKEYDOWN && wp == VK_F4)
			return 0;		// Alt+F4 -> DefWindowProc -> WM_CLOSE
		if (msg == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & (1 << 29)))	// PulseX: Alt+Enter = fullscreen toggle (window management only)
		{
			if (!(lp & (1 << 30)))		// not the auto-repeat
				VID_PC_ToggleFullscreen ();
			*handled = 1;
			return 0;
		}
		key = MapKey (lp);
		if (key == K_ESCAPE)
			in_released_by_esc = 1;
		if (key)
			in_queue (key, true);
		*handled = 1;
		return 0;

	case WM_KEYUP:
	case WM_SYSKEYUP:
		key = MapKey (lp);
		if (key)
			in_queue (key, false);
		*handled = 1;
		return 0;

	case WM_SYSCHAR:
		*handled = 1;		// no beep / menu on Alt+key
		return 0;

	case WM_LBUTTONDOWN: case WM_LBUTTONUP:
	case WM_RBUTTONDOWN: case WM_RBUTTONUP:
	case WM_MBUTTONDOWN: case WM_MBUTTONUP:
		if (in_captured)
		{
			int		down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);

			if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)
				in_queue (K_MOUSE1, down);
			else if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP)
				in_queue (K_MOUSE2, down);
			else
				in_queue (K_MOUSE3, down);
		}
		else if (msg == WM_LBUTTONDOWN && key_dest == key_game)
			in_released_by_esc = 0;		// a click in the window takes the mouse back
		*handled = 1;
		return 0;

	case WM_INPUT:
		{
			RAWINPUT	ri;
			UINT		size = sizeof(ri);

			w_raw_msgs++;
			if (GetRawInputData ((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 && ri.header.dwType == RIM_TYPEMOUSE)
				w_raw_mouse++;
			size = sizeof(ri);
			if (GetRawInputData ((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1
				&& ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)
				&& in_captured)
			{
				mouse_dx += ri.data.mouse.lLastX;
				mouse_dy += ri.data.mouse.lLastY;
				w_accepted++; w_dx_total += ri.data.mouse.lLastX; w_dy_total += ri.data.mouse.lLastY;
			}
		}
		return 0;	// not handled: DefWindowProc must see WM_INPUT

	case WM_KILLFOCUS:
		IN_PC_Activate (0);
		return 0;
	}
	(void)hwnd;
	return 0;
}

/*
================
IN_Init / IN_Shutdown
================
*/
void IN_Init (void)
{
	RAWINPUTDEVICE	rid;
	HWND			hwnd = (HWND)VID_PC_Window ();

	Cvar_RegisterVariable (&_windowed_mouse);
	Cvar_RegisterVariable (&m_filter);
	Cvar_RegisterVariable (&m_freelook);

	if (!hwnd || COM_CheckParm ("-nomouse") || pc_harness || COM_CheckParm ("-quitafter"))
		return;		// no raw input = the cursor is never captured
	rid.usUsagePage = 0x01;		// generic desktop
	rid.usUsage = 0x02;			// mouse
	rid.dwFlags = 0;
	rid.hwndTarget = hwnd;
	in_rawreg = RegisterRawInputDevices (&rid, 1, sizeof(rid)) ? 1 : 0;
	in_active = GetForegroundWindow () == hwnd;
}

void IN_Shutdown (void)
{
	capture (0);
	printf ("PCIN fg_changes=%d raw_registered=%d raw_msgs=%d raw_mouse=%d accepted=%d dx_total=%lld dy_total=%lld move_calls=%d move_captured=%d yaw_applied=%.2f\n",
		w_fg_changes, in_rawreg, w_raw_msgs, w_raw_mouse, w_accepted, w_dx_total, w_dy_total, w_move_calls, w_move_captured, w_yaw_applied);
	fflush (stdout);
}

void IN_Commands (void)
{
}

void IN_ClearStates (void)
{
	mouse_dx = mouse_dy = 0;
	old_mouse_x = old_mouse_y = 0;
}

/*
================
IN_Move

Same arithmetic as id's in_win.c IN_MouseMove, plus m_freelook.
================
*/
void IN_Move (usercmd_t *cmd)
{
	float	mouse_x, mouse_y;
	int		mlook;

	w_move_calls++;
	if (!in_captured)
	{
		mouse_dx = mouse_dy = 0;
		return;
	}
	w_move_captured++;

	if (m_filter.value)
	{
		mouse_x = (mouse_dx + old_mouse_x) * 0.5;
		mouse_y = (mouse_dy + old_mouse_y) * 0.5;
	}
	else
	{
		mouse_x = mouse_dx;
		mouse_y = mouse_dy;
	}
	old_mouse_x = mouse_dx;
	old_mouse_y = mouse_dy;
	mouse_dx = mouse_dy = 0;

	mouse_x *= sensitivity.value;
	mouse_y *= sensitivity.value;

	mlook = (in_mlook.state & 1) || m_freelook.value;

// add mouse X/Y movement to cmd
	if ( (in_strafe.state & 1) || (lookstrafe.value && mlook))
		cmd->sidemove += m_side.value * mouse_x;
	else
		{ cl.viewangles[YAW] -= m_yaw.value * mouse_x; w_yaw_applied += m_yaw.value * mouse_x; }

	if (mlook)
		V_StopPitchDrift ();

	if (mlook && !(in_strafe.state & 1))
	{
		cl.viewangles[PITCH] += m_pitch.value * mouse_y;
		if (cl.viewangles[PITCH] > 80)
			cl.viewangles[PITCH] = 80;
		if (cl.viewangles[PITCH] < -70)
			cl.viewangles[PITCH] = -70;
	}
	else
	{
		if ((in_strafe.state & 1) && noclip_anglehack)
			cmd->upmove -= m_forward.value * mouse_y;
		else
			cmd->forwardmove -= m_forward.value * mouse_y;
	}
}
