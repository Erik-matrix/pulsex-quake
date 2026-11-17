# 2026-10-26: the PulseX changes to id's code, as data. Applies each (file, old, new) to quake\id\
# exactly once; a patch whose "new" text is already there is skipped, so the script can be rerun.
# Line endings: id's files are a mix of CRLF and LF; the patch text is written with \n and converted
# to the file's own line ending before matching (and the file is written back in that ending).
#   python tools/patch_id.py            apply
#   python tools/patch_id.py --check    only report what is applied / missing
import os
import sys

ID = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "id")

PATCHES = []


def P(fname, old, new, count=1):
    PATCHES.append((fname, old, new, count))


# ---------------------------------------------------------------- platform hooks
P("quakedef.h",
  '#include "cdaudio.h"\n',
  '#include "cdaudio.h"\n'
  '#include "pc_prof.h"\t// PulseX: per-stage timers (-profile), see pc\\pc_prof.h\n')

P("menu.c",
  '#ifdef _WIN32\n#include "winquake.h"\n#endif\n',
  '#ifdef _WIN32\n#include "pc_winq.h"\t// PulseX: was winquake.h (DirectDraw/DirectSound/WinSock); menu.c needs only modestate + _windowed_mouse\n#endif\n')

P("quakedef.h",
  '#include <setjmp.h>\n',
  '#include <setjmp.h>\n'
  '#include <stddef.h>\t// PulseX: offsetof\n'
  '#include <stdint.h>\t// PulseX: intptr_t / uintptr_t for the 64-bit pointer fixes\n')

# ---------------------------------------------------------------- 64-bit: pointers through 32-bit 'long'
# On LLP64 Windows 'long' is 32 bits. These round a stack/heap address up to CACHE_SIZE through a
# long: the top 32 bits of the pointer are dropped and the result points nowhere (crash on first use).
P("r_main.c",
  '\t\tr_edges =  (edge_t *)\n\t\t\t\t(((long)&ledges[0] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));\n',
  '\t\tr_edges =  (edge_t *)\t// PulseX: 64-bit, was (long): truncated the pointer\n\t\t\t\t(((uintptr_t)&ledges[0] + CACHE_SIZE - 1) & ~(uintptr_t)(CACHE_SIZE - 1));\n')
P("r_main.c",
  '\t\tsurfaces =  (surf_t *)\n\t\t\t\t(((long)&lsurfs[0] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));\n',
  '\t\tsurfaces =  (surf_t *)\t// PulseX: 64-bit, was (long): truncated the pointer\n\t\t\t\t(((uintptr_t)&lsurfs[0] + CACHE_SIZE - 1) & ~(uintptr_t)(CACHE_SIZE - 1));\n')
P("r_edge.c",
  '\tbasespan_p = (espan_t *)\n\t\t\t((long)(basespans + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));\n',
  '\tbasespan_p = (espan_t *)\t// PulseX: 64-bit, was (long): truncated the pointer\n\t\t\t((uintptr_t)(basespans + CACHE_SIZE - 1) & ~(uintptr_t)(CACHE_SIZE - 1));\n')
P("d_polyse.c",
  '\ta_spans = (spanpackage_t *)\n\t\t\t(((long)&spans[0] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));\n',
  '\ta_spans = (spanpackage_t *)\t// PulseX: 64-bit, was (long): truncated the pointer\n\t\t\t(((uintptr_t)&spans[0] + CACHE_SIZE - 1) & ~(uintptr_t)(CACHE_SIZE - 1));\n')
P("r_alias.c",
  '\tpfinalverts = (finalvert_t *)\n\t\t\t(((long)&finalverts[0] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));\n',
  '\tpfinalverts = (finalvert_t *)\t// PulseX: 64-bit, was (long): truncated the pointer\n\t\t\t(((uintptr_t)&finalverts[0] + CACHE_SIZE - 1) & ~(uintptr_t)(CACHE_SIZE - 1));\n')

# The edge cache stores a byte offset from r_edges and rebuilt the pointer as (unsigned long)r_edges + offset.
# The subtraction (edge_p - r_edges) survives truncation (mod 2^32); the rebuilt pointer does not.
P("r_draw.c",
  '\tpedge_t = (edge_t *)((unsigned long)r_edges + r_pedge->cachededgeoffset);\n',
  '\tpedge_t = (edge_t *)((byte *)r_edges + r_pedge->cachededgeoffset);\t// PulseX: 64-bit, was (unsigned long)\n')
P("r_draw.c",
  '\t\t\t\t\tif ((((unsigned long)edge_p - (unsigned long)r_edges) >\n'
  '\t\t\t\t\t\t r_pedge->cachededgeoffset) &&\n'
  '\t\t\t\t\t\t(((edge_t *)((unsigned long)r_edges +\n'
  '\t\t\t\t\t\t r_pedge->cachededgeoffset))->owner == r_pedge))\n'
  '\t\t\t\t\t{\n'
  '\t\t\t\t\t\tR_EmitCachedEdge ();\n'
  '\t\t\t\t\t\tr_lastvertvalid = false;\n'
  '\t\t\t\t\t\tcontinue;\n'
  '\t\t\t\t\t}\n'
  '\t\t\t\t}\n'
  '\t\t\t}\n'
  '\n'
  '\t\t// assume it\'s cacheable\n'
  '\t\t\tcacheoffset = (byte *)edge_p - (byte *)r_edges;\n'
  '\t\t\tr_leftclipped = r_rightclipped = false;\n'
  '\t\t\tR_ClipEdge (&r_pcurrentvertbase[r_pedge->v[0]],\n',
  '\t\t\t\t\tif ((((byte *)edge_p - (byte *)r_edges) >\t// PulseX: 64-bit, was (unsigned long)\n'
  '\t\t\t\t\t\t r_pedge->cachededgeoffset) &&\n'
  '\t\t\t\t\t\t(((edge_t *)((byte *)r_edges +\n'
  '\t\t\t\t\t\t r_pedge->cachededgeoffset))->owner == r_pedge))\n'
  '\t\t\t\t\t{\n'
  '\t\t\t\t\t\tR_EmitCachedEdge ();\n'
  '\t\t\t\t\t\tr_lastvertvalid = false;\n'
  '\t\t\t\t\t\tcontinue;\n'
  '\t\t\t\t\t}\n'
  '\t\t\t\t}\n'
  '\t\t\t}\n'
  '\n'
  '\t\t// assume it\'s cacheable\n'
  '\t\t\tcacheoffset = (unsigned)((byte *)edge_p - (byte *)r_edges);\n'
  '\t\t\tr_leftclipped = r_rightclipped = false;\n'
  '\t\t\tR_ClipEdge (&r_pcurrentvertbase[r_pedge->v[0]],\n')
P("r_draw.c",
  '\t\t\t\t\tif ((((unsigned long)edge_p - (unsigned long)r_edges) >\n'
  '\t\t\t\t\t\t r_pedge->cachededgeoffset) &&\n'
  '\t\t\t\t\t\t(((edge_t *)((unsigned long)r_edges +\n'
  '\t\t\t\t\t\t r_pedge->cachededgeoffset))->owner == r_pedge))\n'
  '\t\t\t\t\t{\n'
  '\t\t\t\t\t\tR_EmitCachedEdge ();\n'
  '\t\t\t\t\t\tr_lastvertvalid = false;\n'
  '\t\t\t\t\t\tcontinue;\n'
  '\t\t\t\t\t}\n'
  '\t\t\t\t}\n'
  '\t\t\t}\n'
  '\n'
  '\t\t// assume it\'s cacheable\n'
  '\t\t\tcacheoffset = (byte *)edge_p - (byte *)r_edges;\n'
  '\t\t\tr_leftclipped = r_rightclipped = false;\n'
  '\t\t\tR_ClipEdge (&r_pcurrentvertbase[r_pedge->v[1]],\n',
  '\t\t\t\t\tif ((((byte *)edge_p - (byte *)r_edges) >\t// PulseX: 64-bit, was (unsigned long)\n'
  '\t\t\t\t\t\t r_pedge->cachededgeoffset) &&\n'
  '\t\t\t\t\t\t(((edge_t *)((byte *)r_edges +\n'
  '\t\t\t\t\t\t r_pedge->cachededgeoffset))->owner == r_pedge))\n'
  '\t\t\t\t\t{\n'
  '\t\t\t\t\t\tR_EmitCachedEdge ();\n'
  '\t\t\t\t\t\tr_lastvertvalid = false;\n'
  '\t\t\t\t\t\tcontinue;\n'
  '\t\t\t\t\t}\n'
  '\t\t\t\t}\n'
  '\t\t\t}\n'
  '\n'
  '\t\t// assume it\'s cacheable\n'
  '\t\t\tcacheoffset = (unsigned)((byte *)edge_p - (byte *)r_edges);\n'
  '\t\t\tr_leftclipped = r_rightclipped = false;\n'
  '\t\t\tR_ClipEdge (&r_pcurrentvertbase[r_pedge->v[1]],\n')
# 16-bit surface builder (r_pixbytes == 2, never used by this port), same pointer-through-long pattern
P("r_surf.c",
  '\t\tprowdest = (unsigned short *)((long)prowdest + surfrowbytes);\n',
  '\t\tprowdest = (unsigned short *)((byte *)prowdest + surfrowbytes);\t// PulseX: 64-bit, was (long)\n')

# ---------------------------------------------------------------- 64-bit: QuakeC string offsets
# string_t is a 32-bit offset from pr_strings (inside the hunk). "ptr - pr_strings" for a pointer to
# .bss/.data (sv.name, mod_known[].name, pr_string_temp) is ~0x7ff6'xxxx'xxxx away from a heap hunk on
# 64-bit Windows and silently truncates to garbage. PR_SetEngineString checks the range and moves an
# out-of-range string into the hunk (level lifetime, like ED_NewString).
P("progs.h",
  'char\t*ED_NewString (char *string);\n',
  'char\t*ED_NewString (char *string);\n'
  'int\t\tPR_SetEngineString (char *s);\t// PulseX: 64-bit-safe "s - pr_strings"\n')
P("pr_edict.c",
  '/*\n=============\nED_NewString\n=============\n*/\n',
  '/*\n=============\nPR_SetEngineString\n\n'
  'PulseX: 64-bit. string_t is an int offset from pr_strings. A pointer inside the hunk (progs\n'
  'strings, ED_NewString, client names, pr_string_temp) is always within reach. A pointer to static\n'
  'storage is not on a 64-bit build: such a string is copied into the hunk (it lives until the next\n'
  'map, as everything the server hands to QuakeC). Only spawn-time strings take that path.\n'
  '=============\n*/\n'
  'int PR_SetEngineString (char *s)\n'
  '{\n'
  '\tptrdiff_t\td = s - pr_strings;\n'
  '\tchar\t\t*copy;\n'
  '\n'
  '\tif (d == (int)d)\n'
  '\t\treturn (int)d;\n'
  '\tcopy = Hunk_Alloc ((int)strlen (s) + 1);\n'
  '\tstrcpy (copy, s);\n'
  '\td = copy - pr_strings;\n'
  '\tif (d != (int)d)\n'
  '\t\tSys_Error ("PR_SetEngineString: hunk out of reach of pr_strings");\n'
  '\treturn (int)d;\n'
  '}\n'
  '\n'
  '/*\n=============\nED_NewString\n=============\n*/\n')
P("pr_edict.c",
  '\t\t*(string_t *)d = ED_NewString (s) - pr_strings;\n',
  '\t\t*(string_t *)d = PR_SetEngineString (ED_NewString (s));\t// PulseX: 64-bit\n')
P("pr_edict.c",
  '\tpr_strings = (char *)progs + progs->ofs_strings;\n',
  '\tpr_strings = (char *)progs + progs->ofs_strings;\n'
  '\t{\t// PulseX: 64-bit, the temp string builtins return must sit in the hunk, near pr_strings\n'
  '\t\textern char *pr_string_temp;\n'
  '\t\tpr_string_temp = Hunk_AllocName (128, "strtemp");\n'
  '\t}\n')
# edicts are packed at pr_edict_size; edict_t now holds 8-byte pointers (link_t area), keep them aligned
P("pr_edict.c",
  '\tpr_edict_size = progs->entityfields * 4 + sizeof (edict_t) - sizeof(entvars_t);\n',
  '\tpr_edict_size = progs->entityfields * 4 + sizeof (edict_t) - sizeof(entvars_t);\n'
  '\tpr_edict_size = (pr_edict_size + 7) & ~7;\t// PulseX: 64-bit, 8-byte aligned link_t pointers in every edict\n')
P("pr_cmds.c",
  'char\tpr_string_temp[128];\n',
  'char\t*pr_string_temp;\t// PulseX: 64-bit, was char[128] in .bss: out of int range of pr_strings; 128 bytes in the hunk (PR_LoadProgs)\n')
P("pr_cmds.c",
  '\t\tsprintf (pr_string_temp, "%5.1f",v);\n\tG_INT(OFS_RETURN) = pr_string_temp - pr_strings;\n',
  '\t\tsprintf (pr_string_temp, "%5.1f",v);\n\tG_INT(OFS_RETURN) = PR_SetEngineString (pr_string_temp);\t// PulseX: 64-bit\n')
P("pr_cmds.c",
  'G_VECTOR(OFS_PARM0)[1], G_VECTOR(OFS_PARM0)[2]);\n\tG_INT(OFS_RETURN) = pr_string_temp - pr_strings;\n',
  'G_VECTOR(OFS_PARM0)[1], G_VECTOR(OFS_PARM0)[2]);\n\tG_INT(OFS_RETURN) = PR_SetEngineString (pr_string_temp);\t// PulseX: 64-bit\n')
P("pr_cmds.c",
  '\tsprintf (pr_string_temp, "entity %i", G_EDICTNUM(OFS_PARM0));\n\tG_INT(OFS_RETURN) = pr_string_temp - pr_strings;\n',
  '\tsprintf (pr_string_temp, "entity %i", G_EDICTNUM(OFS_PARM0));\n\tG_INT(OFS_RETURN) = PR_SetEngineString (pr_string_temp);\t// PulseX: 64-bit\n')
P("pr_cmds.c",
  '\te->v.model = m - pr_strings;\n',
  '\te->v.model = PR_SetEngineString (m);\t// PulseX: 64-bit\n')
P("sv_main.c",
  '\tent->v.model = sv.worldmodel->name - pr_strings;\n',
  '\tent->v.model = PR_SetEngineString (sv.worldmodel->name);\t// PulseX: 64-bit, mod_known[] is static storage\n')
P("sv_main.c",
  '\tpr_global_struct->mapname = sv.name - pr_strings;\n',
  '\tpr_global_struct->mapname = PR_SetEngineString (sv.name);\t// PulseX: 64-bit, sv is static storage\n')
P("sv_main.c",
  '\tpr_global_struct->startspot = sv.startspot - pr_strings;\n',
  '\tpr_global_struct->startspot = PR_SetEngineString (sv.startspot);\t// PulseX: 64-bit\n')
P("host_cmd.c",
  '\thost_client->edict->v.netname = host_client->name - pr_strings;\n',
  '\thost_client->edict->v.netname = PR_SetEngineString (host_client->name);\t// PulseX: 64-bit\n')
P("host_cmd.c",
  '\t\tent->v.netname = host_client->name - pr_strings;\n',
  '\t\tent->v.netname = PR_SetEngineString (host_client->name);\t// PulseX: 64-bit\n')

# ---------------------------------------------------------------- 64-bit: harmless casts, made explicit
# Low-bit alignment tests and offsetof idioms: the value is right, the cast is not portable C.
P("common.c",
  '\tif ( (((long)dest | count) & 3) == 0)\n',
  '\tif ( (((uintptr_t)dest | count) & 3) == 0)\t// PulseX: 64-bit cast (low bits only)\n')
P("common.c",
  '\tif (( ( (long)dest | (long)src | count) & 3) == 0 )\n',
  '\tif (( ( (uintptr_t)dest | (uintptr_t)src | count) & 3) == 0 )\t// PulseX: 64-bit cast (low bits only)\n')
P("d_fill.c",
  '\tif (((rwidth & 0x03) == 0) && (((long)dest & 0x03) == 0))\n',
  '\tif (((rwidth & 0x03) == 0) && (((uintptr_t)dest & 0x03) == 0))\t// PulseX: 64-bit cast (low bits only)\n')
P("d_scan.c",
  '\t\tif ((long)pdest & 0x02)\n',
  '\t\tif ((uintptr_t)pdest & 0x02)\t// PulseX: 64-bit cast (low bits only)\n')
P("r_main.c",
  '\tif ( (long)(&dummy) & 3 )\n',
  '\tif ( (uintptr_t)(&dummy) & 3 )\t// PulseX: 64-bit cast (low bits only)\n')
P("r_main.c",
  '\tif ( (long)(&r_warpbuffer) & 3 )\n',
  '\tif ( (uintptr_t)(&r_warpbuffer) & 3 )\t// PulseX: 64-bit cast (low bits only)\n')
P("d_edge.c",
  '\t\t\tD_DrawSolidSurface (s, (int)s->data & 0xFF);\n',
  '\t\t\tD_DrawSolidSurface (s, (int)(intptr_t)s->data & 0xFF);\t// PulseX: 64-bit cast (r_drawflat colour = low pointer bits)\n')
P("common.h",
  '#define\tSTRUCT_FROM_LINK(l,t,m) ((t *)((byte *)l - (int)&(((t *)0)->m)))\n',
  '#define\tSTRUCT_FROM_LINK(l,t,m) ((t *)((byte *)l - offsetof(t,m)))\t// PulseX: 64-bit, was (int)&((t *)0)->m\n')
# surfcache_t holds pointers; blocks were rounded to 4 bytes, leaving the next header misaligned
P("d_surf.c",
  '\tsize = (int)&((surfcache_t *)0)->data[size];\n\tsize = (size + 3) & ~3;\n',
  '\tsize = (int)(offsetof (surfcache_t, data) + size);\t// PulseX: 64-bit, was (int)&((surfcache_t *)0)->data[size]\n'
  '\tsize = (size + 7) & ~7;\t// PulseX: 64-bit, was 4: surfcache_t starts with two pointers\n')
# VCR (-record/-playback debugging): the socket pointer is only an identity token in the file; the
# low 32 bits are written and compared consistently (net_vcr.c reads the low half of driverdata)
P("net_main.c",
  'vcrGetMessage.session = (long)sock;\n',
  'vcrGetMessage.session = (long)(intptr_t)sock;\t// PulseX: 64-bit cast (identity token, low 32 bits)\n', 2)
P("net_main.c",
  'vcrSendMessage.session = (long)sock;\n',
  'vcrSendMessage.session = (long)(intptr_t)sock;\t// PulseX: 64-bit cast (identity token, low 32 bits)\n', 3)
P("net_main.c",
  '\t\t\t\tvcrConnect.session = (long)ret;\n',
  '\t\t\t\tvcrConnect.session = (long)(intptr_t)ret;\t// PulseX: 64-bit cast (identity token, low 32 bits, as net_vcr.c reads it)\n')

# ---------------------------------------------------------------- real bugs the warnings found (not 64-bit)
# chase.c calls SV_RecursiveHullCheck without a prototype: 0 and 1 go in integer registers while the
# callee reads its float parameters from float registers (and on x86 the bits of int 1 are 1.4e-45f).
P("chase.c",
  '#include "quakedef.h"\n',
  '#include "quakedef.h"\n\n'
  '// PulseX: was called without a prototype; the float arguments 0 and 1 arrived as garbage\n'
  'qboolean SV_RecursiveHullCheck (hull_t *hull, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, trace_t *trace);\n')
# a level's "message" was used as a printf format string ('%' in a map message = crash)
P("sv_main.c",
  '\tsprintf (message, pr_strings+sv.edicts->v.message);\n',
  '\tsprintf (message, "%s", pr_strings+sv.edicts->v.message);\t// PulseX: was the format string itself\n')
# 2000 << 24 overflows int (undefined behaviour); every compiler id used folded it to 0xD0000000 - keep that value
P("r_edge.c",
  '\tedge_sentinel.u = 2000 << 24;\t\t// make sure nothing sorts past this\n',
  '\tedge_sentinel.u = (int)(2000u << 24);\t\t// make sure nothing sorts past this\t// PulseX: was 2000 << 24 (signed overflow, UB); same 32-bit value\n')
# abs() on floats: an implicit float->int truncation. Kept exactly (it is id's monster-chase logic on every
# platform), but spelled out so it is visibly deliberate
P("sv_move.c",
  '\tif ( ((rand()&3) & 1) ||  abs(deltay)>abs(deltax))\n',
  '\tif ( ((rand()&3) & 1) ||  abs((int)deltay)>abs((int)deltax))\t// PulseX: id truncates the floats here; now explicit\n')
# console.c's Con_DebugLog uses open/write/close; with _MSC_VER defined it included nothing for them
P("console.c",
  '#ifndef _MSC_VER\n#include <unistd.h>\n#endif\n',
  '#ifndef _MSC_VER\n#include <unistd.h>\n#else\n#include <io.h>\t// PulseX: open/write/close were implicitly declared\n#endif\n')

# ---------------------------------------------------------------- -profile probes (pc\pc_prof.h)
# A function-like macro with the function's own name wraps every call after it in the file; the call
# inside the macro is not re-expanded, so it reaches the real function.
P("host.c",
  '\tSCR_UpdateScreen ();\n\n\tif (host_speeds.value)\n\t\ttime2 = Sys_FloatTime ();\n',
  '\tPROF_PUSH (PS_SCREEN);\t// PulseX: profile\n\tSCR_UpdateScreen ();\n\tPROF_POP ();\t// PulseX: profile\n\n\tif (host_speeds.value)\n\t\ttime2 = Sys_FloatTime ();\n')
P("r_main.c",
  '#include "quakedef.h"\n#include "r_local.h"\n',
  '#include "quakedef.h"\n#include "r_local.h"\n\n'
  '// PulseX: -profile probes (pc\\pc_prof.h); these functions live in other files\n'
  '#define R_RenderWorld()\t\tdo { PROF_PUSH (PS_BSP); R_RenderWorld (); PROF_POP (); } while (0)\n'
  '#define R_ScanEdges()\t\tdo { PROF_PUSH (PS_EDGES); R_ScanEdges (); PROF_POP (); } while (0)\n'
  '#define R_AliasDrawModel(l)\tdo { PROF_PUSH (PS_ALIAS); R_AliasDrawModel (l); PROF_POP (); } while (0)\n'
  '#define R_DrawSprite()\t\tdo { PROF_PUSH (PS_SPRITE); R_DrawSprite (); PROF_POP (); } while (0)\n'
  '#define R_DrawParticles()\tdo { PROF_PUSH (PS_PARTICLE); R_DrawParticles (); PROF_POP (); } while (0)\n'
  '#define D_WarpScreen()\t\tdo { PROF_PUSH (PS_WARP); D_WarpScreen (); PROF_POP (); } while (0)\n')
P("r_main.c",
  '/*\n================\nR_EdgeDrawing\n================\n*/\n',
  '// PulseX: -profile probe; defined above, so the macro starts here\n'
  '#define R_DrawBEntitiesOnList()\tdo { PROF_PUSH (PS_BSP); R_DrawBEntitiesOnList (); PROF_POP (); } while (0)\n\n'
  '/*\n================\nR_EdgeDrawing\n================\n*/\n')
P("r_main.c",
  '\tR_RenderView_ ();\n}\n',
  '\tPROF_PUSH (PS_RVIEW);\t// PulseX: profile\n\tR_RenderView_ ();\n\tPROF_POP ();\t// PulseX: profile\n}\n')
P("r_edge.c",
  '#include "quakedef.h"\n#include "r_local.h"\n',
  '#include "quakedef.h"\n#include "r_local.h"\n\n'
  '// PulseX: -profile probe (pc\\pc_prof.h)\n'
  '#define D_DrawSurfaces()\tdo { PROF_PUSH (PS_DSURF); D_DrawSurfaces (); PROF_POP (); } while (0)\n')
P("d_edge.c",
  '#include "quakedef.h"\n#include "d_local.h"\n',
  '#include "quakedef.h"\n#include "d_local.h"\n\n'
  '// PulseX: -profile probes (pc\\pc_prof.h); these functions live in other files\n'
  '#define D_DrawZSpans(p)\t\tdo { PROF_PUSH (PS_ZSPANS); D_DrawZSpans (p); PROF_POP (); } while (0)\n'
  '#define Turbulent8(p)\t\tdo { PROF_PUSH (PS_TURB); Turbulent8 (p); PROF_POP (); } while (0)\n'
  '#define D_DrawSkyScans8(p)\tdo { PROF_PUSH (PS_SKY); D_DrawSkyScans8 (p); PROF_POP (); } while (0)\n'
  '#define R_MakeSky()\t\t\tdo { PROF_PUSH (PS_SKY); R_MakeSky (); PROF_POP (); } while (0)\n'
  '#define D_CacheSurface(f,m)\t({ surfcache_t *pc_c_; PROF_PUSH (PS_SURFCACHE); pc_c_ = D_CacheSurface (f, m); PROF_POP (); pc_c_; })\n')
P("d_edge.c",
  '\t\t\t\t(*d_drawspans) (s->spans);\n',
  '\t\t\t\tPROF_PUSH (PS_SPANS);\t// PulseX: profile\n\t\t\t\t(*d_drawspans) (s->spans);\n\t\t\t\tPROF_POP ();\t// PulseX: profile\n')
P("d_surf.c",
  '\tc_surf++;\n\tR_DrawSurface ();\n',
  '\tc_surf++;\n\tPROF_PUSH (PS_SURFBUILD);\t// PulseX: profile\n\tR_DrawSurface ();\n\tPROF_POP ();\t// PulseX: profile\n')


def main():
    check = "--check" in sys.argv
    bad = 0
    for fname, old, new, count in PATCHES:
        path = os.path.join(ID, fname)
        with open(path, "rb") as f:
            data = f.read().decode("latin-1")
        crlf = "\r\n" in data
        o = old.replace("\n", "\r\n") if crlf else old
        n = new.replace("\n", "\r\n") if crlf else new
        if data.count(n) >= count and (o not in data or o in n):
            print("applied  %-12s %s" % (fname, old.split("\n")[0][:60]))
            continue
        c = data.count(o)
        if c != count:
            print("FAILED   %-12s (%d matches) %s" % (fname, c, old.split("\n")[0][:60]))
            bad += 1
            continue
        if check:
            print("missing  %-12s %s" % (fname, old.split("\n")[0][:60]))
            bad += 1
            continue
        data = data.replace(o, n)
        with open(path, "wb") as f:
            f.write(data.encode("latin-1"))
        print("patched  %-12s %s" % (fname, old.split("\n")[0][:60]))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
