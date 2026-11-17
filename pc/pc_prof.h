/*
Copyright (C) 2026 the PulseX authors. Part of the PulseX Quake port, GPL v2 or later (see LICENSE).

pc_prof.h -- PulseX: per-stage CPU timers for the software renderer.

Enabled at runtime by "-profile" (pc_prof_on). Off, every probe is one predictable branch.
The timers are EXCLUSIVE: a stage pushed inside another stage pauses the outer one, so
D_DrawSpans8 called from inside R_ScanEdges -> D_DrawSurfaces is booked as "spans", not as
"edges". The bottom of the stack is PS_HOST (everything not inside a named stage).
Time source: QueryPerformanceCounter (on Snapdragon X = the 19.2 MHz generic timer, 52 ns).
*/
#ifndef PC_PROF_H
#define PC_PROF_H

typedef enum
{
	PS_HOST = 0,	// rest of Host_Frame: input, demo/net parse, client update, game logic
	PS_SCREEN,		// SCR_UpdateScreen minus the 3D view: status bar, console, menus, view setup
	PS_RVIEW,		// R_RenderView minus the named stages below (frame setup, mark leaves, ...)
	PS_BSP,			// R_RenderWorld + R_DrawBEntitiesOnList: BSP walk, clipping, edge emission
	PS_EDGES,		// R_ScanEdges: active-edge sort, span generation (minus surface drawing)
	PS_DSURF,		// D_DrawSurfaces bookkeeping: gradients, bmodel transforms, solid fills
	PS_SURFCACHE,	// D_CacheSurface: cache lookup and allocation (minus the build below)
	PS_SURFBUILD,	// R_DrawSurface: building a lit, mipped surface into the cache
	PS_SPANS,		// D_DrawSpans8: perspective texture mapping of world spans
	PS_ZSPANS,		// D_DrawZSpans: z-buffer fill for world spans
	PS_TURB,		// Turbulent8: water/slime/lava/teleport spans
	PS_SKY,			// D_DrawSkyScans8 + R_MakeSky
	PS_ALIAS,		// R_AliasDrawModel (entities + view model) incl. D_PolysetDraw
	PS_SPRITE,		// R_DrawSprite
	PS_PARTICLE,	// R_DrawParticles
	PS_WARP,		// D_WarpScreen (underwater)
	PS_PALCONV,		// vid_pc: 8-bit + palette -> BGRA8 into the D3D11 upload texture
	PS_PRESENT,		// vid_pc: draw the scaled quad + Present
	PS_DUMP,		// harness: writing -framedump files (not part of a real frame)
	PS_COUNT
} pcstage_t;

extern int pc_prof_on;
extern const char *pc_stage_name[PS_COUNT];

void PC_ProfPush (int stage);
void PC_ProfPop (void);

#define PROF_PUSH(s)	do { if (pc_prof_on) PC_ProfPush (s); } while (0)
#define PROF_POP()		do { if (pc_prof_on) PC_ProfPop (); } while (0)

#endif
