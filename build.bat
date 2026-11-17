@echo off
rem PulseX Quake: builds bin\pulsex-quake.exe (Windows 11 on ARM64).
rem id's WinQuake software renderer (id\, C only: id386 = 0, no .s files) + our platform layer (pc\) + the host side
rem of the DSP bridge (host\) and the qaic stub (dsp\pxquake_stub.c).
rem
rem Needs (environment variables, with defaults):
rem   VCVARSARM64       vcvarsarm64.bat of Visual Studio 2022 / Build Tools (the Windows SDK + MSVC libraries).
rem                     Leave it unset if you already run in an "ARM64 Native Tools" prompt.
rem   LLVM_BIN          folder with clang.exe, lld-link and llvm-rc.exe   (default: %ProgramFiles%\LLVM\bin)
rem   HEXAGON_SDK_ROOT  Hexagon SDK 5.x (only its headers are used here)  (default: C:\Qualcomm\Hexagon_SDK\5.5.7.0)
rem
rem   -fno-strict-aliasing : Quake reads floats as ints through casts all over (LittleFloat, BSP loaders).
rem   -fcommon             : id declares globals in headers without extern (tentative definitions).
rem   -ffp-contract=off    : no fused multiply-add. The CPU frames are the reference the NPU path is compared
rem                          against pixel for pixel; contraction choices would move with every rebuild.
setlocal enabledelayedexpansion
if defined VCVARSARM64 call "%VCVARSARM64%" >nul 2>&1
if not defined LLVM_BIN set "LLVM_BIN=%ProgramFiles%\LLVM\bin"
if not defined HEXAGON_SDK_ROOT set "HEXAGON_SDK_ROOT=C:\Qualcomm\Hexagon_SDK\5.5.7.0"
if not exist "%LLVM_BIN%\clang.exe" ( echo clang.exe not found in "%LLVM_BIN%" - set LLVM_BIN & exit /b 5 )
if not exist "%HEXAGON_SDK_ROOT%\incs" ( echo Hexagon SDK not found in "%HEXAGON_SDK_ROOT%" - set HEXAGON_SDK_ROOT & exit /b 5 )
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist build mkdir build
set "CC=%LLVM_BIN%\clang.exe"
set FL=--target=arm64-pc-windows-msvc -march=armv8.7a+fp16+dotprod+i8mm -O2 -DNDEBUG -D_DLL -D_MT -Xclang --dependent-lib=msvcrt
set CF=%FL% -std=gnu17 -fno-strict-aliasing -ffp-contract=off -fcommon -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -Iid -Ipc -Ihost
rem Warnings: everything that can be a real bug stays on (pointer/int size, formats, uninitialized,
rem implicit declarations); the id-style noise (unused locals, missing braces, if/else layout) is off.
set WF=-Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wno-missing-braces -Wno-dangling-else -Wno-parentheses -Wno-misleading-indentation -Wno-pointer-sign -Wno-unknown-pragmas -Wno-trigraphs -Wno-deprecated-non-prototype -Werror=implicit-function-declaration -Werror=int-conversion -Werror=pointer-to-int-cast -Werror=int-to-pointer-cast -Werror=incompatible-pointer-types
set OBJS=
for %%f in (id\*.c pc\*.c) do (
  "%CC%" %CF% %WF% -c %%f -o build\%%~nf.obj || exit /b 1
  set OBJS=!OBJS! build\%%~nf.obj
)
rem The DSP bridge and the qaic stub are compiled apart from Quake with the Hexagon SDK headers (their typedefs
rem must not meet id's).
set DF=%FL% -std=gnu17 -ffp-contract=off -DWINNT -D__QAIC_REMOTE_EXPORT= -D_CRT_SECURE_NO_WARNINGS -I"%HEXAGON_SDK_ROOT%\incs" -I"%HEXAGON_SDK_ROOT%\incs\stddef" -I"%HEXAGON_SDK_ROOT%\ipc\fastrpc\rpcmem\inc" -Idsp -Ihost -Wno-unused-function
"%CC%" %DF% -c host\pc_dsp.c -o build\pc_dsp.obj || exit /b 4
"%CC%" %DF% -c dsp\pxquake_stub.c -o build\pxquake_stub.obj || exit /b 4
set OBJS=!OBJS! build\pc_dsp.obj build\pxquake_stub.obj
"%LLVM_BIN%\llvm-rc.exe" /FO build\pulsex_quake.res pc\pulsex_quake.rc || exit /b 3
"%CC%" %FL% -nostartfiles -nostdlib -fuse-ld=lld-link -Xlinker /SUBSYSTEM:WINDOWS -Xlinker /ENTRY:mainCRTStartup -Xlinker /MANIFESTINPUT:pc/gui.manifest -Xlinker /MANIFEST:EMBED ^
 %OBJS% build\pulsex_quake.res -o bin\pulsex-quake.exe ^
 -ld3d11.lib -ldxgi.lib -ld3dcompiler.lib -ldxguid.lib -lwinmm.lib -lkernel32 -luser32 -lgdi32 -lole32 -luuid -ladvapi32 -loldnames || exit /b 2
echo built bin\pulsex-quake.exe
