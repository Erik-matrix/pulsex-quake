# PulseX Quake

id Software's Quake (1996) with its software renderer running on the **Hexagon NPU** of a Snapdragon X laptop
(Windows 11 on ARM64). The CPU runs the game, BSP/PVS and the edge sort. The NPU's vector units (HVX) draw the spans
(texture × light, z-buffer) and the turbulent water. The frames are **bit-identical** to id's CPU renderer, and that is
checked pixel for pixel. The game is playable with keyboard and mouse.

It is a hobby project: an experiment in what a laptop NPU can do outside neural networks. It is not affiliated with
id Software, ZeniMax/Bethesda or Qualcomm. Quake is a trademark of ZeniMax Media.

Licence: **GNU GPL v2 or later** (`LICENSE`, id's `gnu.txt`). The game data is **not** included. You need `pak0.pak`
from the free shareware Quake 1.06, or from your own copy of the game.

## How it works

| stage | where |
|---|---|
| game logic, QuakeC, BSP, PVS, edge sorting, surface-cache building | CPU (id's C code) |
| span drawing: texel stepping, `vgather` from a VTCM copy of the surface cache, z-buffer | **NPU, HVX**, 3 worker threads |
| turbulent water (`Turbulent8`) | **NPU, HVX** (`vlut32`) |
| models (alias) | CPU, recorded and replayed with the z-test after the NPU spans |
| palette → colour | GPU (D3D11 pixel shader, 1 byte per pixel uploaded) |

- **Over frames:** the NPU draws frame N into its own buffer while the CPU prepares N+1.
- **Hand-off:** a 4-slot ring in shared memory (`fastrpc_mmap` / `HAP_mmap_get`) to a persistent DSP thread. It costs
  0.023 ms per frame, against 0.155 ms for a FastRPC call.
- **Timing:** the server runs fixed 1/72 s ticks, the same physics as id's 72 Hz, verified bit for bit. The picture
  runs at the display's rate or at `host_maxfps`, with entity interpolation.

Measured on one Snapdragon X laptop, timedemo `demo1`, headless:

| | CPU (id's renderer) | NPU path |
|---|---|---|
| 640×480 | 1440 fps | 1564 fps (+8–11 %, `-dspspans 4`) |
| 1280×960 | 461 fps | 511 fps (+9–11 %, `-dspspans 3`) |

In a window the display and present cap both paths, so a game frame costs the same either way. The point is the
experiment: the frames are exact, and the CPU is left free.

## The NPU module and signing (read this first)

Windows loads a DSP module (`libpxquake_skel.so`) only with a **signed catalog** (`libpxquake_skel.cat`) beside it.
Production modules are signed through Microsoft's attestation service. This module is not; it has only been
signed with a self-made certificate, so **on a normal machine the NPU path does not start.** The game then says
`PCDSP NPU UNAVAILABLE` and plays on the CPU. Everything else works the same.

`dsp/sign_catalog.ps1` signs the catalog with a certificate of your own. Whether Windows accepts it depends on your
machine's trust settings. This repo changes none of them, and does not tell you to.

## Build

Needs Visual Studio 2022 Build Tools (ARM64, Windows SDK), LLVM/clang for Windows on ARM, and the Hexagon SDK 5.x
(its headers, and hexagon-clang for the DSP side). Set the paths if they are not the defaults:

```
set LLVM_BIN=C:\Program Files\LLVM\bin
set HEXAGON_SDK_ROOT=C:\Qualcomm\Hexagon_SDK\5.5.7.0
set VCVARSARM64=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsarm64.bat
build.bat                       -> bin\pulsex-quake.exe
bash dsp/build_skel.sh          -> bin\libpxquake_skel.so   (SIGN=1 also writes the .cat with your certificate)
```

The game builds warning-free with `-Wall`, and pointer/int size mixing is an error. `-ffp-contract=off` is on both
sides: the CPU frames are the reference for the NPU frames.

## Run

```
bin\pulsex-quake.exe -basedir C:\Games\Quake                                 CPU renderer
bin\pulsex-quake.exe -basedir C:\Games\Quake -dspspans 4 -width 1280 -height 960   NPU renderer
```

`-basedir` is the folder that holds `id1\pak0.pak`. Esc opens the menu. Mouse look is on, and the cursor is captured
while you play. The window can be resized: the image is scaled nearest-neighbour and keeps its aspect ratio. The key
left of 1 opens the console.

| option | effect |
|---|---|
| `-dspspans 0…4` | 0 = CPU (default), 1 = scalar DSP, 2 = HVX, 3 = HVX pipeline over FastRPC, **4 = HVX pipeline over the shared-memory ring** |
| `-dspturb 0` | water on the CPU |
| `-dsprecycle <s>` | renew the DSP session every *s* seconds (default 60; see Known issues) |
| `+host_maxfps <n>` / `-novsync` | picture rate (default 0 = the display's rate) |
| `-pcgpupal 0` | palette on the CPU instead of the GPU |

## Known issues

- **The CDSP subsystem can crash** after ~3–4 minutes of continuous play with one long-lived DSP session (the
  System log says `CDSP subsystem Crashed`). The default `-dsprecycle 60` renews the session every minute: 9 ms, not
  visible. With it, 10.5 minutes at 120 fps ran clean.
- **If the NPU stops answering**, the game notices within 2 s and goes on drawing on the CPU. The window title then
  says so. After a CDSP crash, Windows refuses new NPU sessions until the computer is restarted.
- No sound mixing on the NPU (sound is id's mixer on the CPU), no networking beyond loopback (single player).

## Test harness

| option | effect |
|---|---|
| `-timedemo demo1` | plays the demo as fast as possible, prints Quake's line plus `PCRESULT ... fps=`; headless unless `-window` |
| `-framedump <dir> <N>` | every Nth frame as `fNNNNNN.raw` (8-bit indices) + `.pal`; implies `-fixeddt 1/72`, so the dumps are bit-reproducible |
| `-profile` | per-stage timers (spans, surfaces, edges, models, palette, present) |
| `-selftest <dir>` | starts e1m1 with a server, checks the 64-bit fixes, writes `selftest.png` |

`python tools\compare_dumps.py <dirA> <dirB>` compares two dump folders. A CPU dump and an NPU dump of the same demo
are identical.

## What is id's and what is ours

| path | origin |
|---|---|
| `id/` | **id Software**: `WinQuake/` from <https://github.com/id-Software/Quake> (commit `bf4ac424`), the software-renderer subset in C. Every change is marked `// PulseX:`; `tools/patch_id.py` lists them, including 12 fixes for 64-bit pointers. |
| `pc/` | the platform layer: D3D11 window, raw-input mouse, timing, harness, the span hand-off to the DSP (`d_dspspans.c`), the frame pipeline (`pc_pipe.c`) |
| `host/` | the host side of the DSP bridge (FastRPC session, shared-memory ring, shutdown) |
| `dsp/` | the DSP module: `pxquake_imp.c` + `pq_*.h` (HVX kernels, the mailbox thread); `pxquake.idl` and the qaic-generated glue |

The fixed-tick host loop follows the approach of QuakeSpasm-Spiked and ironwail (no code taken from them).
