/*
Copyright (C) 2026 the PulseX authors. Part of the PulseX Quake port, GPL v2 or later (see LICENSE).

vid_pc.c -- PulseX video driver: the software renderer's 8-bit framebuffer in a Win32 window.

Replaces id's vid_win.c (DirectDraw / MGL / DIB modes, mode lists, the video menu). The renderer
always draws at ONE fixed size (default 640x480, -width/-height up to MAXWIDTH x MAXHEIGHT).
Each frame the 8-bit buffer goes through the current palette to BGRA8 (stage "palconv"), is
uploaded into a D3D11 dynamic texture and drawn as one triangle with POINT sampling (nearest
neighbour) into the largest viewport with the render aspect that fits the window ("present").
The window can be resized freely; that only changes the scale, never the rendered pixels.
With -headless (default for -timedemo) no window or device is created at all.
*/

#include "quakedef.h"
#include "d_local.h"
#include "pc_harness.h"
#include "pc_winq.h"
#include "pc_dsp.h"	// PulseX phase 2
#include "pc_pipe.h"	// PulseX phase 4

int		pc_dspspans;	// PulseX phase 2: -dspspans 1 = world spans + z drawn by the DSP
static int	pc_npu_locked;	// PulseX phase 9: -dspspans was asked for, but the NPU session did not open

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>

viddef_t		vid;				// global video state
unsigned short	d_8to16table[256];
unsigned		d_8to24table[256];	// palette as BGRA8 (B in the low byte), what the texture wants
modestate_t		modestate = MS_WINDOWED;
byte			pc_palette[768];

#define	DEFAULT_WIDTH	640
#define	DEFAULT_HEIGHT	480

static byte		*vid_buffer;
static short	*vid_zbuffer;
static byte		*vid_surfcache;

static HWND						pc_hwnd;
static int						pc_minimized;
static ID3D11Device				*dev;
static ID3D11DeviceContext		*ctx;
static IDXGISwapChain1			*swap;
static ID3D11RenderTargetView	*rtv;
static ID3D11Texture2D			*frametex;
static ID3D11ShaderResourceView	*framesrv;
static ID3D11VertexShader		*vs;
static ID3D11PixelShader		*ps;
static ID3D11SamplerState		*point_sampler;
static int						allow_tearing;
static int						sync_interval;
static int						client_w, client_h;
/* PulseX phase 10 */
static int						pc_gpupal = 1;		// -pcgpupal 0 = the CPU palconv
static int						pc_paltest;			// -pcgpupaltest
static int						pc_palsick;			// -pcgpupalsick (known-sick)
static ID3D11Texture2D			*idxtex;			// the 8-bit frame, R8_UNORM
static ID3D11ShaderResourceView	*idxsrv;
static ID3D11Buffer				*palbuf;			// 256 x R32_UINT (BGRA as d_8to24table)
static ID3D11ShaderResourceView	*palsrv;
static ID3D11PixelShader			*ps_pal;
static unsigned				pal_version = 1, pal_uploaded = 0;
static double					up_bytes, up_frames, pal_uploads;
static ID3D11Texture2D			*pt_tex[2], *pt_stage[2];	// -pcgpupaltest: off-screen CPU / GPU results
static ID3D11RenderTargetView	*pt_rtv[2];
static int						pt_w, pt_h;
static long					pt_frames, pt_same, pt_diff, pt_shifted, pt_palettes;
static double					pt_pixels_diff;
static unsigned				pt_base_hash, pt_last_hash;
static float					*pace_ms;			// Present-to-Present intervals (interactive)
static long					pace_n, pace_gaps;
static double					pace_last;
static int						pc_refresh_hz;
void VID_PC_Report (void);
double PC_HostMaxFpsNow (void);

static const char shader_src[] =
	"Texture2D t : register(t0);\n"
	"SamplerState s : register(s0);\n"
	"struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
	"V vsmain (uint id : SV_VertexID) {\n"
	"  V o; o.uv = float2((id << 1) & 2, id & 2);\n"
	"  o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
	"float4 psmain (V i) : SV_Target { return t.Sample(s, i.uv); }\n"
	"Texture2D<float> ti : register(t1);\n"
	"Buffer<uint> pal : register(t2);\n"
	"float4 psmain_pal (V i) : SV_Target {\n"
	"  uint k = (uint)(ti.Sample(s, i.uv) * 255.0 + 0.5);\n"
	"  uint c = pal[k];\n"
	"  return float4(((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0, 1); }\n";

void VID_LockBuffer (void) {}
void VID_UnlockBuffer (void) {}

void *VID_PC_Window (void)
{
	return pc_hwnd;
}

int VID_PC_VSync (void)	// PulseX phase 10: Present waits for the display
{
	return pc_hwnd && swap && !pc_minimized && sync_interval == 1;
}
int VID_PC_RefreshHz (void)	// PulseX phase 10: the refresh rate of the window's monitor (0 = unknown)
{
	if (!pc_refresh_hz)
	{
		MONITORINFOEXW	mi;
		DEVMODEW		dm;
		HMONITOR		m = MonitorFromWindow (pc_hwnd ? pc_hwnd : GetDesktopWindow (), MONITOR_DEFAULTTOPRIMARY);
		memset (&mi, 0, sizeof(mi));
		memset (&dm, 0, sizeof(dm));
		mi.cbSize = sizeof(mi);
		dm.dmSize = sizeof(dm);
		if (GetMonitorInfoW (m, (MONITORINFO *)&mi) && EnumDisplaySettingsW (mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
			pc_refresh_hz = (int)dm.dmDisplayFrequency;
		else
			pc_refresh_hz = -1;
		printf ("PCVID display refresh %d Hz\n", pc_refresh_hz);
	}
	return pc_refresh_hz > 0 ? pc_refresh_hz : 0;
}

/*
================
palette
================
*/
void VID_SetPalette (unsigned char *palette)
{
	int		i;

	memcpy (pc_palette, palette, 768);
	pal_version++;	// PulseX phase 10: the GPU palette is uploaded again
	for (i = 0 ; i < 256 ; i++)
		d_8to24table[i] = 0xff000000u | ((unsigned)palette[i*3+0] << 16)
						| ((unsigned)palette[i*3+1] << 8) | (unsigned)palette[i*3+2];
}

void VID_ShiftPalette (unsigned char *palette)
{
	VID_SetPalette (palette);
}

/*
================
D3D11
================
*/
static void release_rtv (void)
{
	if (rtv)
	{
		ID3D11RenderTargetView_Release (rtv);
		rtv = NULL;
	}
}

static void make_rtv (void)
{
	ID3D11Texture2D	*back = NULL;

	if (FAILED (IDXGISwapChain1_GetBuffer (swap, 0, &IID_ID3D11Texture2D, (void **)&back)))
		Sys_Error ("VID: GetBuffer failed");
	if (FAILED (ID3D11Device_CreateRenderTargetView (dev, (ID3D11Resource *)back, NULL, &rtv)))
		Sys_Error ("VID: CreateRenderTargetView failed");
	ID3D11Texture2D_Release (back);
}

static void resize_swapchain (int w, int h)
{
	if (!swap || w <= 0 || h <= 0 || (w == client_w && h == client_h))
		return;
	client_w = w;
	client_h = h;
	ID3D11DeviceContext_OMSetRenderTargets (ctx, 0, NULL, NULL);
	release_rtv ();
	if (FAILED (IDXGISwapChain1_ResizeBuffers (swap, 0, w, h, DXGI_FORMAT_UNKNOWN,
				allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)))
		Sys_Error ("VID: ResizeBuffers failed");
	make_rtv ();
}

static ID3DBlob *compile (const char *entry, const char *target)
{
	ID3DBlob	*code = NULL, *err = NULL;

	if (FAILED (D3DCompile (shader_src, sizeof(shader_src) - 1, "vid_pc", NULL, NULL, entry, target,
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
		Sys_Error ("VID: shader %s: %s", entry, err ? (char *)ID3D10Blob_GetBufferPointer (err) : "?");
	if (err)
		ID3D10Blob_Release (err);
	return code;
}

static void init_d3d (void)
{
	DXGI_SWAP_CHAIN_DESC1	sd;
	D3D11_TEXTURE2D_DESC	td;
	D3D11_SAMPLER_DESC		smp;
	IDXGIFactory2			*factory = NULL;
	IDXGIFactory5			*factory5 = NULL;
	ID3DBlob				*blob;
	D3D_FEATURE_LEVEL		fl = D3D_FEATURE_LEVEL_11_0;
	BOOL					tear = FALSE;
	RECT					rc;

	if (FAILED (D3D11CreateDevice (NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1,
				D3D11_SDK_VERSION, &dev, NULL, &ctx)))
		Sys_Error ("VID: D3D11CreateDevice failed");
	if (FAILED (CreateDXGIFactory2 (0, &IID_IDXGIFactory2, (void **)&factory)))
		Sys_Error ("VID: CreateDXGIFactory2 failed");
	if (SUCCEEDED (IDXGIFactory2_QueryInterface (factory, &IID_IDXGIFactory5, (void **)&factory5)))
	{
		if (SUCCEEDED (IDXGIFactory5_CheckFeatureSupport (factory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING,
					&tear, sizeof(tear))) && tear)
			allow_tearing = 1;
		IDXGIFactory5_Release (factory5);
	}

	GetClientRect (pc_hwnd, &rc);
	client_w = rc.right - rc.left;
	client_h = rc.bottom - rc.top;

	memset (&sd, 0, sizeof(sd));
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 3;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Flags = allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
	if (FAILED (IDXGIFactory2_CreateSwapChainForHwnd (factory, (IUnknown *)dev, pc_hwnd, &sd, NULL, NULL, &swap)))
		Sys_Error ("VID: CreateSwapChainForHwnd failed");
	IDXGIFactory2_MakeWindowAssociation (factory, pc_hwnd, DXGI_MWA_NO_ALT_ENTER);
	IDXGIFactory2_Release (factory);
	make_rtv ();

	memset (&td, 0, sizeof(td));
	td.Width = vid.width;
	td.Height = vid.height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DYNAMIC;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED (ID3D11Device_CreateTexture2D (dev, &td, NULL, &frametex)))
		Sys_Error ("VID: CreateTexture2D failed");
	if (FAILED (ID3D11Device_CreateShaderResourceView (dev, (ID3D11Resource *)frametex, NULL, &framesrv)))
		Sys_Error ("VID: CreateShaderResourceView failed");

	pc_gpupal = 1;	// PulseX phase 10: the palette on the GPU (default)
	{	int t = COM_CheckParm ("-pcgpupal");
		if (t && t < com_argc-1)
			pc_gpupal = Q_atoi (com_argv[t+1]) ? 1 : 0;
		pc_palsick = COM_CheckParm ("-pcgpupalsick") ? 1 : 0;
		t = COM_CheckParm ("-pcgpupaltest");
		pc_paltest = t ? (t < com_argc-1 && Q_atoi (com_argv[t+1]) == 2 ? 2 : 1) : 0;
	}
	td.Format = DXGI_FORMAT_R8_UNORM;
	if (FAILED (ID3D11Device_CreateTexture2D (dev, &td, NULL, &idxtex)) ||
		FAILED (ID3D11Device_CreateShaderResourceView (dev, (ID3D11Resource *)idxtex, NULL, &idxsrv)))
		Sys_Error ("VID: the R8 index texture failed");
	{
		D3D11_BUFFER_DESC				bd;
		D3D11_SHADER_RESOURCE_VIEW_DESC	sv;
		memset (&bd, 0, sizeof(bd));
		bd.ByteWidth = 256 * 4;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		memset (&sv, 0, sizeof(sv));
		sv.Format = DXGI_FORMAT_R32_UINT;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.FirstElement = 0;
		sv.Buffer.NumElements = 256;
		if (FAILED (ID3D11Device_CreateBuffer (dev, &bd, NULL, &palbuf)) ||
			FAILED (ID3D11Device_CreateShaderResourceView (dev, (ID3D11Resource *)palbuf, &sv, &palsrv)))
			Sys_Error ("VID: the palette buffer failed");
	}
	blob = compile ("psmain_pal", "ps_5_0");
	ID3D11Device_CreatePixelShader (dev, ID3D10Blob_GetBufferPointer (blob), ID3D10Blob_GetBufferSize (blob), NULL, &ps_pal);
	ID3D10Blob_Release (blob);
	if (!ps_pal)
		Sys_Error ("VID: the palette shader failed");
	printf ("PCVID palette on the %s%s\n", pc_gpupal ? "GPU (R8 index + 256-entry buffer)" : "CPU (palconv, BGRA8 upload)", pc_paltest ? ", -pcgpupaltest: both paths compared every frame" : "");
	blob = compile ("vsmain", "vs_5_0");
	ID3D11Device_CreateVertexShader (dev, ID3D10Blob_GetBufferPointer (blob), ID3D10Blob_GetBufferSize (blob), NULL, &vs);
	ID3D10Blob_Release (blob);
	blob = compile ("psmain", "ps_5_0");
	ID3D11Device_CreatePixelShader (dev, ID3D10Blob_GetBufferPointer (blob), ID3D10Blob_GetBufferSize (blob), NULL, &ps);
	ID3D10Blob_Release (blob);
	if (!vs || !ps)
		Sys_Error ("VID: CreateShader failed");

	memset (&smp, 0, sizeof(smp));
	smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;		// nearest neighbour
	smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	smp.MaxLOD = D3D11_FLOAT32_MAX;
	if (FAILED (ID3D11Device_CreateSamplerState (dev, &smp, &point_sampler)))
		Sys_Error ("VID: CreateSamplerState failed");

	// interactive: vsync (Quake itself holds 72 Hz); timedemo in a window: as fast as it goes
	sync_interval = cls.timedemo || COM_CheckParm ("-timedemo") ? 0 : 1;
	if (COM_CheckParm ("-novsync"))
		sync_interval = 0;
	{	IDXGIDevice1	*dd = NULL;	// PulseX phase 10: at most one frame queued (vsync paces, one frame of latency)
		if (SUCCEEDED (ID3D11Device_QueryInterface (dev, &IID_IDXGIDevice1, (void **)&dd)))
		{
			IDXGIDevice1_SetMaximumFrameLatency (dd, 1);
			IDXGIDevice1_Release (dd);
		}
	}
}

static void shutdown_d3d (void)
{
	if (ctx) ID3D11DeviceContext_ClearState (ctx);
	if (point_sampler) ID3D11SamplerState_Release (point_sampler);
	{	int i;	// PulseX phase 10
		for (i = 0 ; i < 2 ; i++)
		{
			if (pt_rtv[i]) ID3D11RenderTargetView_Release (pt_rtv[i]);
			if (pt_tex[i]) ID3D11Texture2D_Release (pt_tex[i]);
			if (pt_stage[i]) ID3D11Texture2D_Release (pt_stage[i]);
			pt_rtv[i] = NULL; pt_tex[i] = NULL; pt_stage[i] = NULL;
		}
		if (ps_pal) ID3D11PixelShader_Release (ps_pal);
		if (palsrv) ID3D11ShaderResourceView_Release (palsrv);
		if (palbuf) ID3D11Buffer_Release (palbuf);
		if (idxsrv) ID3D11ShaderResourceView_Release (idxsrv);
		if (idxtex) ID3D11Texture2D_Release (idxtex);
		ps_pal = NULL; palsrv = NULL; palbuf = NULL; idxsrv = NULL; idxtex = NULL;
	}
	if (ps) ID3D11PixelShader_Release (ps);
	if (vs) ID3D11VertexShader_Release (vs);
	if (framesrv) ID3D11ShaderResourceView_Release (framesrv);
	if (frametex) ID3D11Texture2D_Release (frametex);
	release_rtv ();
	if (swap) IDXGISwapChain1_Release (swap);
	if (ctx) ID3D11DeviceContext_Release (ctx);
	if (dev) ID3D11Device_Release (dev);
	point_sampler = NULL; ps = NULL; vs = NULL; framesrv = NULL; frametex = NULL;
	swap = NULL; ctx = NULL; dev = NULL;
}

/*
================
window
================
*/
static volatile int	pc_close_req;	// PulseX phase 7: a harness WM_CLOSE, acted on after the message loop
void IN_PC_DrainEvents (void);
static LRESULT pc_wndproc_inner (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK pc_wndproc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	LRESULT	r;

	pcdsp_msg_enter ();	// PulseX phase 7: a DSP RPC from in here is counted and reported (it must never happen)
	r = pc_wndproc_inner (hwnd, msg, wp, lp);
	pcdsp_msg_leave ();
	return r;
}
static LRESULT pc_wndproc_inner (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	int		handled = 0;
	LRESULT	r;

	r = (LRESULT)IN_PC_WndProc (hwnd, msg, wp, lp, &handled);
	if (handled)
		return r;

	switch (msg)
	{
	case WM_SIZE:
		pc_minimized = (wp == SIZE_MINIMIZED);
		if (!pc_minimized)
			resize_swapchain (LOWORD (lp), HIWORD (lp));
		return 0;
	case WM_ACTIVATE:
		IN_PC_Activate (LOWORD (wp) != WA_INACTIVE && !HIWORD (wp));
		return 0;
	case WM_CLOSE:
		if (pc_harness)
		{
			pc_close_req = 2;		// a harness run never writes config.cfg; it leaves after the message loop
			return 0;
		}
		Cbuf_AddText ("quit\n");	// text only: the command runs in Host_Frame, never in here
		return 0;
	case WM_ERASEBKGND:
		return 1;
	}
	return DefWindowProcW (hwnd, msg, wp, lp);
}

static int	pc_start_fullscreen;
void VID_PC_ToggleFullscreen (void);
static void create_window (void)
{
	WNDCLASSEXW	wc;
	RECT		rc;
	DWORD		style = WS_OVERLAPPEDWINDOW;
	int			scale, sw, sh;

	memset (&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = pc_wndproc;
	wc.hInstance = GetModuleHandleW (NULL);
	wc.hCursor = LoadCursorW (NULL, (LPCWSTR)IDC_ARROW);
	wc.hIcon = LoadIconW (wc.hInstance, MAKEINTRESOURCEW (1));
	wc.hbrBackground = (HBRUSH)GetStockObject (BLACK_BRUSH);
	wc.lpszClassName = L"PulseXQuake";
	RegisterClassExW (&wc);

	// start at the largest integer scale that fits the work area (crisp pixels), at least 1x
	sw = GetSystemMetrics (SM_CXMAXIMIZED) - 64;
	sh = GetSystemMetrics (SM_CYMAXIMIZED) - 96;
	scale = 1;
	while ((int)vid.width * (scale+1) <= sw && (int)vid.height * (scale+1) <= sh)
		scale++;
	rc.left = rc.top = 0;
	rc.right = vid.width * scale;
	rc.bottom = vid.height * scale;
	AdjustWindowRect (&rc, style, FALSE);

	pc_hwnd = CreateWindowExW (0, wc.lpszClassName, pc_dspspans ? L"PulseX Quake \u2013 NPU" : pc_npu_locked ? L"PulseX Quake \u2013 CPU (NPU l\u00e5st: starta om)" : L"PulseX Quake", style,	// PulseX: the title says when the NPU draws
				CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
				NULL, NULL, wc.hInstance, NULL);
	if (!pc_hwnd)
		Sys_Error ("VID: CreateWindow failed");
	init_d3d ();
	if (COM_CheckParm ("-fullscreen") && !pc_harness)	// PulseX: start borderless fullscreen (Alt+Enter toggles)
		pc_start_fullscreen = 1;
	if (pc_harness || COM_CheckParm ("-quitafter") || COM_CheckParm ("-pcnoactivate"))	// PulseX phase 7: -pcnoactivate = the close test
	{
		// test runs never take the keyboard focus from whatever the user is doing
		ShowWindow (pc_hwnd, SW_SHOWNOACTIVATE);
		UpdateWindow (pc_hwnd);
	}
	else
	{
		ShowWindow (pc_hwnd, SW_SHOWNORMAL);
		UpdateWindow (pc_hwnd);
		SetForegroundWindow (pc_hwnd);
	}
	if (pc_start_fullscreen)
		VID_PC_ToggleFullscreen ();
}

// PulseX 2026-10-27: Alt+Enter, borderless fullscreen on the window's monitor and back (Raymond Chen's pattern)
static int				pc_fullscreen;
static WINDOWPLACEMENT	pc_wp_saved = { sizeof(WINDOWPLACEMENT) };
void VID_PC_ToggleFullscreen (void)
{
	pc_refresh_hz = 0;	// PulseX phase 10: read again (another monitor / mode)
	DWORD		style;
	MONITORINFO	mi;

	if (!pc_hwnd)
		return;
	style = (DWORD)GetWindowLongW (pc_hwnd, GWL_STYLE);
	if (!pc_fullscreen)
	{
		mi.cbSize = sizeof(mi);
		if (!GetWindowPlacement (pc_hwnd, &pc_wp_saved) || !GetMonitorInfoW (MonitorFromWindow (pc_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
			return;
		SetWindowLongW (pc_hwnd, GWL_STYLE, (LONG)(style & ~WS_OVERLAPPEDWINDOW));
		SetWindowPos (pc_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
			mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		pc_fullscreen = 1;
		printf ("PCVID fullscreen 1 (%ldx%ld)\n", mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
	}
	else
	{
		SetWindowLongW (pc_hwnd, GWL_STYLE, (LONG)(style | WS_OVERLAPPEDWINDOW));
		SetWindowPlacement (pc_hwnd, &pc_wp_saved);
		SetWindowPos (pc_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		pc_fullscreen = 0;
		printf ("PCVID fullscreen 0\n");
	}
	fflush (stdout);
}

void VID_PC_PumpMessages (void)
{
	MSG		msg;

	while (PeekMessageW (&msg, NULL, 0, 0, PM_REMOVE))
	{
		TranslateMessage (&msg);
		DispatchMessageW (&msg);
	}
	IN_PC_DrainEvents ();	// PulseX phase 7: the queued keys reach the game here, outside every window procedure
	pcdsp_tick ();			// PulseX phase 7: -dspqwarm (off by default)
	if (pc_dspspans && pcdsp_lost ())	// PulseX phase 9: the DSP stopped answering - the rest of the session on the CPU
	{
		PC_PipeDisable ();
		pc_dspspans = 0;
		printf ("PCDSP the game renders on the CPU from now on (pipeline off, -dspspans 0 path)\n");
		fflush (stdout);
		if (pc_hwnd)
			SetWindowTextW (pc_hwnd, L"PulseX Quake \u2013 CPU (NPU:n svarade inte)");
	}
	if (pc_close_req)
		PC_HarnessExit (pc_close_req);
	IN_PC_Frame ();
}

/*
================
VID_Init
================
*/
void VID_Init (unsigned char *palette)
{
	int		w = DEFAULT_WIDTH, h = DEFAULT_HEIGHT, t, cachesize;

	t = COM_CheckParm ("-width");
	if (t && t < com_argc-1)
		w = Q_atoi (com_argv[t+1]);
	t = COM_CheckParm ("-height");
	if (t && t < com_argc-1)
		h = Q_atoi (com_argv[t+1]);
	if (w < 320) w = 320;
	if (h < 200) h = 200;
	if (w > MAXWIDTH) w = MAXWIDTH;
	if (h > MAXHEIGHT) h = MAXHEIGHT;
	w &= ~7;		// keep rows a multiple of 8 bytes

	vid.width = vid.conwidth = w;
	vid.height = vid.conheight = h;
	vid.maxwarpwidth = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);
	vid.numpages = 1;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));
	vid.rowbytes = vid.conrowbytes = w;
	vid.recalc_refdef = 1;

	cachesize = D_SurfaceCacheForRes (w, h);
	t = COM_CheckParm ("-dspspans");	// PulseX phase 2
	pc_dspspans = (t && t < com_argc-1) ? Q_atoi (com_argv[t+1]) : 0;
	if (pc_dspspans)
	{
		if (pc_dspspans == 3)	// PulseX phase 4: the pipeline (a worker thread owns the DSP session)
			pcdsp_set_pipe (1);
		if (pc_dspspans == 4)	// PulseX phase 5: the pipeline over the persistent DSP worker (dspqueue)
		{
			pcdsp_set_dq (1);
			{
				int qs = COM_CheckParm ("-dspqspin"), qp = COM_CheckParm ("-dspqpoll");	// PulseX phase 5
				pcdsp_set_mb_timing (qs && qs < com_argc-1 ? Q_atoi (com_argv[qs+1]) : -1, qp && qp < com_argc-1 ? Q_atoi (com_argv[qp+1]) : 0);
			}
			{	// TEST -dspqjointest N: bit 0 the DSP mailbox thread skips qurt_thread_exit; bit 1 the old stop flow (ENOTHREAD); bit 2 the HVX workers skip it
				int jt = COM_CheckParm ("-dspqjointest");
				if (jt)
					pcdsp_set_join_test (jt < com_argc-1 ? Q_atoi (com_argv[jt+1]) : 1);
			}
			t = COM_CheckParm ("-dsprecycle");	// PulseX phase 9: recycle the DSP side every N s (DEFAULT 60; 0 = off: the CDSP then crashed after ~3-4 min)
			if (t && t < com_argc-1)
				pcdsp_set_recycle ((int)(Q_atof (com_argv[t+1]) * 1000.0f));
			t = COM_CheckParm ("-dspclock");	// PulseX phase 9: turbo (default) | nom | dcvs - the DSP clock vote (experiment A)
			if (t && t < com_argc-1)
			{
				char *m = com_argv[t+1];
				if (!Q_strcmp (m, "turbo")) pcdsp_set_clock (0);
				else if (!Q_strcmp (m, "nom")) pcdsp_set_clock (1);
				else if (!Q_strcmp (m, "dcvs")) pcdsp_set_clock (2);
				else Sys_Error ("-dspclock %s: use turbo, nom or dcvs", m);
			}
			t = COM_CheckParm ("-dsplpm");	// PulseX phase 7: DSP low-power modes off in the vote (experiment)
			if (t && t < com_argc-1)
				pcdsp_set_lpm (Q_atoi (com_argv[t+1]));
			t = COM_CheckParm ("-dspqspinidle");	// PulseX phase 7: default 1 = the DSP thread never sleeps while idle; 0 = the old sleeping poll
			if (t && t < com_argc-1)
				pcdsp_set_spinidle (Q_atoi (com_argv[t+1]));
			t = COM_CheckParm ("-pctest_dsplost");	// PulseX phase 9, known-sick: the DSP 'stops answering' at frame N
			if (t && t < com_argc-1)
				pcdsp_set_test_lost (Q_atoi (com_argv[t+1]), COM_CheckParm ("-pctest_dsplost_noack") != 0);
			t = COM_CheckParm ("-dsplat");	// PulseX phase 7: lever 2, a DSP wakeup-latency vote in us (experiment)
			if (t && t < com_argc-1)
				pcdsp_set_lat (Q_atoi (com_argv[t+1]));
			t = COM_CheckParm ("-dspqwarm");	// PulseX phase 7: keep-warm, a workaround to measure (default off)
			if (t && t < com_argc-1)
				pcdsp_set_keepwarm (Q_atoi (com_argv[t+1]));
			if (COM_CheckParm ("-dspqnoflush"))
				pcdsp_set_dq_noflush (1);
		}
		if (pc_harness || COM_CheckParm ("-pcnodialog"))	// PulseX phase 7: a stuck shutdown is only logged in tests
			pcdsp_set_dialog (0);
		t = COM_CheckParm ("-dspturb");	// PulseX phase 6: 0 = the CPU water path (default: the water goes to the DSP)
		if (t && t < com_argc-1)
			pcdsp_set_turb (Q_atoi (com_argv[t+1]));
		t = COM_CheckParm ("-dspturbhvx");	// PulseX phase 6: 0 = the DSP's scalar water loop (default HVX)
		if (t && t < com_argc-1)
			pcdsp_set_turb_hvx (Q_atoi (com_argv[t+1]));
		if (COM_CheckParm ("-dspturbsick"))	// PulseX phase 6, known-sick: texel 0 of every water texture the DSP gets is changed
			pcdsp_set_turb_sick (1);
		if (COM_CheckParm ("-pctest_npuopenfail") || pcdsp_open ())	// PulseX phase 9: -pctest_npuopenfail = known-sick
		{
			if (pc_harness)		// a test must not pass on the CPU unnoticed
				Sys_Error ("-dspspans 1: the DSP session did not open: %s", pcdsp_error ());
			printf ("PCDSP NPU UNAVAILABLE: the DSP session did not open (%s) - playing on the CPU. After a CDSP crash "
				"(System log: 'CDSP subsystem Crashed') every new NPU session is refused until the computer is restarted.\n",
				pcdsp_error ()[0] ? pcdsp_error () : "test");
			fflush (stdout);
			pc_npu_locked = 1;
			pc_dspspans = 0;
			goto pc_cpu_buffers;
		}
		vid_buffer = pcdsp_alloc (w * h);
		vid_zbuffer = pcdsp_alloc (w * h * sizeof(short));
		t = COM_CheckParm ("-dspcache");	// PulseX phase 2: surface cache x N in DSP mode (fewer guard flushes)
		if (t && t < com_argc-1 && Q_atoi (com_argv[t+1]) > 1)
			cachesize *= Q_atoi (com_argv[t+1]);
		vid_surfcache = pcdsp_alloc (cachesize);
		if (!vid_buffer || !vid_zbuffer || !vid_surfcache)
			Sys_Error ("-dspspans 1: no shared memory: %s", pcdsp_error ());
		pcdsp_set_buffers (vid_buffer, w * h, vid_zbuffer, w * h, vid_surfcache, cachesize);
		if (pc_dspspans >= 2)	// PulseX phase 3: the HVX kernels (phase 4: also under the pipeline)
			pcdsp_set_hvx (1);
		if (pc_dspspans >= 3)	// PulseX phase 4: two world buffers (fb + z) for the frames in flight (phase 5: also mode 4)
		{
			byte	*w0 = pcdsp_alloc (w * h), *w1 = pcdsp_alloc (w * h);
			short	*z0 = pcdsp_alloc (w * h * sizeof(short)), *z1 = pcdsp_alloc (w * h * sizeof(short));
			if (!w0 || !w1 || !z0 || !z1)
				Sys_Error ("-dspspans 3: no shared memory: %s", pcdsp_error ());
			PC_PipeInit (vid_buffer, vid_zbuffer, w0, z0, w1, z1, w, h);
			t = COM_CheckParm ("-dsppipesick");	// PulseX phase 4: known-sick control of the pipeline
			if (t && t < com_argc-1)
			{
				extern int pc_pipe_sick;
				pc_pipe_sick = Q_atoi (com_argv[t+1]);
			}
		}
		t = COM_CheckParm ("-dspthreads");	// PulseX phase 2: 0 = serial DSP path
		if (t && t < com_argc-1)
			pcdsp_set_threads (Q_atoi (com_argv[t+1]));
		t = COM_CheckParm ("-dspdiag");	// PulseX phase 2: DIAGNOSTIC timing only (wrong pictures)
		if (t && t < com_argc-1)
			pcdsp_set_diag (Q_atoi (com_argv[t+1]));
		t = COM_CheckParm ("-dspvtcm");	// PulseX phase 2: 1 = surface blocks staged into VTCM
		if (t && t < com_argc-1)
			pcdsp_set_vtcm (Q_atoi (com_argv[t+1]));
		t = COM_CheckParm ("-dspinplaceforce");	// PulseX phase 2: test the changed-bytes branch
		if (t && t < com_argc-1)
		{
			extern int pc_inplace_force;
			pc_inplace_force = Q_atoi (com_argv[t+1]);
		}
		t = COM_CheckParm ("-dspmirror");	// PulseX phase 3: 0 = stage the blocks per call
		if (t && t < com_argc-1)
			pcdsp_set_mirror (Q_atoi (com_argv[t+1]));
		t = COM_CheckParm ("-dspmirrorcheck");	// PulseX phase 4: test of the mirror (synchronous calls)
		if (t && t < com_argc-1)
			pcdsp_set_mirrorcheck (Q_atoi (com_argv[t+1]));
		if (COM_CheckParm ("-dspnohdrdirty"))	// PulseX phase 4: known-sick control of the mirror check
			pcdsp_test_nohdr (1);
		t = COM_CheckParm ("-dspsick");
		if (t && t < com_argc-1)
			pcdsp_set_sick (Q_atoi (com_argv[t+1]));
		printf ("PCDSP mode=dspspans width=%d height=%d cache=%d sick_from=%d\n", w, h, cachesize, t ? Q_atoi (com_argv[t+1]) : -1);
	}
	else
	{
pc_cpu_buffers:	// PulseX phase 9: also when the NPU session could not be opened
	vid_buffer = calloc (1, w * h);
	vid_zbuffer = calloc (1, w * h * sizeof(short));
	cachesize = D_SurfaceCacheForRes (w, h);
	vid_surfcache = calloc (1, cachesize);
	}	// PulseX phase 2
	if (!vid_buffer || !vid_zbuffer || !vid_surfcache)
		Sys_Error ("VID: out of memory for %dx%d", w, h);
	vid.buffer = vid.conbuffer = vid_buffer;
	vid.direct = NULL;
	d_pzbuffer = vid_zbuffer;
	D_InitCaches (vid_surfcache, cachesize);

	VID_SetPalette (palette);

	if (!pc_headless)
		create_window ();
	if (pc_npu_locked)
		Con_Printf ("\nNPU:n svarar inte (last efter en krasch).\nSpelet kor pa CPU:n. Starta om datorn\nfor att fa tillbaka NPU:n.\n\n");
	// stdout only: text in the console would show up in frame dumps and differ between -window and headless
	printf ("VID: %dx%d %s, surface cache %d KB, present sync %d tearing %d\n", w, h,
		pc_headless ? "headless" : "window + D3D11", cachesize / 1024, sync_interval, allow_tearing);
}

void VID_Shutdown (void)
{
	PC_PipeDrain ();	// PulseX phase 4: the pending frame is finished (its DSP job waited for) before the session closes
	PC_PipeStats ();
	pcdsp_close ();	// PulseX phase 2: every exit path of the port comes through here (idempotent)
	VID_PC_Report ();	// PulseX phase 10
	shutdown_d3d ();
	if (pc_hwnd)
	{
		DestroyWindow (pc_hwnd);
		pc_hwnd = NULL;
	}
}

/*
================
VID_Update

The whole buffer goes up every frame; the dirty rectangles only mattered for DirectDraw.
================
*/
static void present_from (const byte *buf);
void PC_VidPresentFrom (const byte *buf)	// PulseX phase 4: the pipeline presents a frame when it is finished
{
	present_from (buf);
}

void VID_Update (vrect_t *rects)
{
	if (PC_PipeCurrentPending ())	// PulseX phase 4: this frame is presented when the pipeline finishes it
		return;
	present_from (vid_buffer);
}

static void upload_cpu (const byte *buf);
static void upload_gpu (const byte *buf);
static void draw_quad (ID3D11RenderTargetView *target, int gpu, int w, int h);
static void paltest_frame (const byte *buf);
static void present_from (const byte *buf)
{
	if (!pc_hwnd || !swap || pc_minimized)
		return;
	if (pc_paltest)
		paltest_frame (buf);
	PROF_PUSH (PS_PALCONV);
	if (pc_gpupal)
		upload_gpu (buf);
	else
		upload_cpu (buf);
	PROF_POP ();
	PROF_PUSH (PS_PRESENT);
	draw_quad (rtv, pc_gpupal, client_w, client_h);
	IDXGISwapChain1_Present (swap, sync_interval,
		(sync_interval == 0 && allow_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
	PROF_POP ();
	if (!cls.timedemo)	// PulseX phase 10: frame pacing (interactive)
	{
		const double	t = PC_Seconds ();
		if (!pace_ms)
			pace_ms = malloc (sizeof(float) * 400000);
		if (pace_last > 0 && pace_ms)
		{
			const double	ms = (t - pace_last) * 1000.0;
			if (ms > 100.0)
				pace_gaps++;	// minimized, a load, a menu pause: not pacing
			else if (pace_n < 400000)
				pace_ms[pace_n++] = (float)ms;
		}
		pace_last = t;
	}
}
static void upload_gpu (const byte *buf)
{
	D3D11_MAPPED_SUBRESOURCE	map;
	unsigned					y;
	if (SUCCEEDED (ID3D11DeviceContext_Map (ctx, (ID3D11Resource *)idxtex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
	{
		for (y = 0 ; y < vid.height ; y++)
			memcpy ((byte *)map.pData + (size_t)y * map.RowPitch, buf + y * vid.rowbytes, vid.width);
		ID3D11DeviceContext_Unmap (ctx, (ID3D11Resource *)idxtex, 0);
	}
	up_bytes += (double)vid.width * vid.height;
	if (pal_uploaded != pal_version && SUCCEEDED (ID3D11DeviceContext_Map (ctx, (ID3D11Resource *)palbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
	{
		memcpy (map.pData, d_8to24table, 256 * 4);
		if (pc_palsick)
			((unsigned *)map.pData)[15] ^= 0x00010101u;	// known-sick (-pcgpupalsick)
		ID3D11DeviceContext_Unmap (ctx, (ID3D11Resource *)palbuf, 0);
		pal_uploaded = pal_version;
		up_bytes += 1024;
		pal_uploads++;
	}
	up_frames++;
}
static void upload_cpu (const byte *buf)
{
	D3D11_MAPPED_SUBRESOURCE	map;
	unsigned					x, y;
	if (SUCCEEDED (ID3D11DeviceContext_Map (ctx, (ID3D11Resource *)frametex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
	{
		for (y = 0 ; y < vid.height ; y++)
		{
			const byte	*src = buf + y * vid.rowbytes;	// PulseX phase 4: the given screen (F)
			unsigned	*dst = (unsigned *)((byte *)map.pData + (size_t)y * map.RowPitch);

			for (x = 0 ; x < vid.width ; x++)
				dst[x] = d_8to24table[src[x]];
		}
		ID3D11DeviceContext_Unmap (ctx, (ID3D11Resource *)frametex, 0);
	}
	up_bytes += (double)vid.width * vid.height * 4;
	up_frames++;
}
static void draw_quad (ID3D11RenderTargetView *target, int gpu, int w, int h)
{
	D3D11_VIEWPORT				vp;
	static const float			black[4] = {0, 0, 0, 1};
	float						s;
	ID3D11ShaderResourceView	*srvs[3];
	// largest viewport with the render aspect (square pixels) inside the client area
	s = (float)w / vid.width;
	if ((float)h / vid.height < s)
		s = (float)h / vid.height;
	vp.Width = vid.width * s;
	vp.Height = vid.height * s;
	vp.TopLeftX = (w - vp.Width) * 0.5f;
	vp.TopLeftY = (h - vp.Height) * 0.5f;
	vp.MinDepth = 0;
	vp.MaxDepth = 1;

	ID3D11DeviceContext_OMSetRenderTargets (ctx, 1, &target, NULL);
	ID3D11DeviceContext_ClearRenderTargetView (ctx, target, black);
	ID3D11DeviceContext_RSSetViewports (ctx, 1, &vp);
	ID3D11DeviceContext_IASetPrimitiveTopology (ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_IASetInputLayout (ctx, NULL);
	ID3D11DeviceContext_VSSetShader (ctx, vs, NULL, 0);
	ID3D11DeviceContext_PSSetShader (ctx, gpu ? ps_pal : ps, NULL, 0);
	srvs[0] = framesrv; srvs[1] = idxsrv; srvs[2] = palsrv;
	ID3D11DeviceContext_PSSetShaderResources (ctx, 0, 3, srvs);
	ID3D11DeviceContext_PSSetSamplers (ctx, 0, 1, &point_sampler);
	ID3D11DeviceContext_Draw (ctx, 3, 0);
}
/* -pcgpupaltest: the CPU path and the GPU path into two off-screen targets the size of the window, read back,
   compared byte for byte (the viewport, the scale and the sampler are the presented ones) */
static unsigned pal_hash (void)
{
	unsigned	h = 2166136261u;
	int			i;
	for (i = 0 ; i < 256 ; i++)
		h = (h ^ d_8to24table[i]) * 16777619u;
	return h;
}
static void paltest_frame (const byte *buf)
{
	D3D11_TEXTURE2D_DESC		td;
	D3D11_MAPPED_SUBRESOURCE	m0, m1;
	unsigned					hsh = pal_hash ();
	int							i, y, x, same = 1;
	const int	tw = pc_paltest == 2 ? (int)vid.width * 137 / 100 + 3 : client_w;
	const int	th = pc_paltest == 2 ? (int)vid.height * 137 / 100 + 1 : client_h;
	if (pt_w != tw || pt_h != th)
	{
		for (i = 0 ; i < 2 ; i++)
		{
			if (pt_rtv[i]) ID3D11RenderTargetView_Release (pt_rtv[i]);
			if (pt_tex[i]) ID3D11Texture2D_Release (pt_tex[i]);
			if (pt_stage[i]) ID3D11Texture2D_Release (pt_stage[i]);
			memset (&td, 0, sizeof(td));
			td.Width = tw; td.Height = th; td.MipLevels = 1; td.ArraySize = 1;
			td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
			if (FAILED (ID3D11Device_CreateTexture2D (dev, &td, NULL, &pt_tex[i])) ||
				FAILED (ID3D11Device_CreateRenderTargetView (dev, (ID3D11Resource *)pt_tex[i], NULL, &pt_rtv[i])))
				Sys_Error ("VID: paltest target");
			td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED (ID3D11Device_CreateTexture2D (dev, &td, NULL, &pt_stage[i])))
				Sys_Error ("VID: paltest staging");
		}
		pt_w = tw; pt_h = th;
	}
	upload_cpu (buf);
	upload_gpu (buf);
	draw_quad (pt_rtv[0], 0, pt_w, pt_h);
	draw_quad (pt_rtv[1], 1, pt_w, pt_h);
	for (i = 0 ; i < 2 ; i++)
		ID3D11DeviceContext_CopyResource (ctx, (ID3D11Resource *)pt_stage[i], (ID3D11Resource *)pt_tex[i]);
	if (FAILED (ID3D11DeviceContext_Map (ctx, (ID3D11Resource *)pt_stage[0], 0, D3D11_MAP_READ, 0, &m0)))
		return;
	if (FAILED (ID3D11DeviceContext_Map (ctx, (ID3D11Resource *)pt_stage[1], 0, D3D11_MAP_READ, 0, &m1)))
	{
		ID3D11DeviceContext_Unmap (ctx, (ID3D11Resource *)pt_stage[0], 0);
		return;
	}
	for (y = 0 ; y < pt_h ; y++)
	{
		const unsigned	*a = (const unsigned *)((const byte *)m0.pData + (size_t)y * m0.RowPitch);
		const unsigned	*b = (const unsigned *)((const byte *)m1.pData + (size_t)y * m1.RowPitch);
		if (memcmp (a, b, (size_t)pt_w * 4))
		{
			same = 0;
			for (x = 0 ; x < pt_w ; x++)
				if (a[x] != b[x])
					pt_pixels_diff++;
		}
	}
	ID3D11DeviceContext_Unmap (ctx, (ID3D11Resource *)pt_stage[1], 0);
	ID3D11DeviceContext_Unmap (ctx, (ID3D11Resource *)pt_stage[0], 0);
	if (!pt_frames)
		pt_base_hash = hsh;
	if (hsh != pt_base_hash)
		pt_shifted++;
	if (!pt_frames || hsh != pt_last_hash)
		pt_palettes++;
	pt_last_hash = hsh;
	pt_frames++;
	if (same) pt_same++; else pt_diff++;
}
static int cmp_f (const void *a, const void *b)
{
	const float	x = *(const float *)a, y = *(const float *)b;
	return x < y ? -1 : x > y ? 1 : 0;
}
void VID_PC_Report (void)	// PulseX phase 10: upload, palette test and pacing, once at the end
{
	static int	done;
	if (done || !dev)
		return;
	done = 1;
	printf ("PCVID upload path=%s frames=%.0f bytes_per_frame=%.0f palette_uploads=%.0f\n", pc_gpupal ? "gpu" : "cpu",
		up_frames, up_frames ? up_bytes / up_frames : 0.0, pal_uploads);
	if (pc_paltest)
		printf ("PCVID paltest frames=%ld identical=%ld differing=%ld pixels_differing=%.0f frames_with_a_shifted_palette=%ld palettes_seen=%ld size=%dx%d -> %s\n",
			pt_frames, pt_same, pt_diff, pt_pixels_diff, pt_shifted, pt_palettes, pt_w, pt_h,
			pt_frames && !pt_diff ? "IDENTICAL" : pt_frames ? "DIFFERS" : "NO FRAMES");
	if (pace_n > 10)
	{
		static const float	edge[] = { 6, 7, 8, 8.5f, 9, 10, 12.5f, 17, 25, 100 };
		long				hist[11], i, over = 0;
		double				sum = 0;
		const double		target = 1000.0 / PC_HostMaxFpsNow ();
		long				late1 = 0;
		memset (hist, 0, sizeof(hist));
		for (i = 0 ; i < pace_n ; i++)
		{
			int	k = 0;
			sum += pace_ms[i];
			while (k < 10 && pace_ms[i] >= edge[k])
				k++;
			hist[k]++;
			if (pace_ms[i] > target * 1.5)
				over++;
			if (pace_ms[i] > target + 1.0)
				late1++;
		}
		qsort (pace_ms, pace_n, sizeof(float), cmp_f);
		printf ("PCVID pacing frames=%ld mean_ms=%.3f p50=%.3f p99=%.3f max=%.3f target_ms=%.3f (host_maxfps %.0f, display %d Hz, vsync %d) missed(>1.5x)=%ld (%.2f%%) late(>target+1ms)=%ld (%.2f%%) gaps>100ms=%ld\n",
			pace_n, sum / pace_n, pace_ms[pace_n / 2], pace_ms[(long)(pace_n * 0.99)], pace_ms[pace_n - 1], target, PC_HostMaxFpsNow (), VID_PC_RefreshHz (), sync_interval,
			over, 100.0 * over / pace_n, late1, 100.0 * late1 / pace_n, pace_gaps);
		printf ("PCVID pacing hist_ms <6:%ld 6-7:%ld 7-8:%ld 8-8.5:%ld 8.5-9:%ld 9-10:%ld 10-12.5:%ld 12.5-17:%ld 17-25:%ld 25-100:%ld >=100:%ld\n",
			hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], hist[8], hist[9], hist[10]);
	}
	fflush (stdout);
}

void VID_HandlePause (qboolean pause)
{
	// the pause key: in_pc.c releases the cursor while paused (IN_PC_Frame looks at cl.paused)
}

int VID_SetMode (int modenum, unsigned char *palette)
{
	return 1;	// one fixed mode
}

/*
================
D_BeginDirectRect / D_EndDirectRect

The loading disc was drawn straight to the DirectDraw surface; there is none here.
================
*/
void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height)
{
}

void D_EndDirectRect (int x, int y, int width, int height)
{
}
