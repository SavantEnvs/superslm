@echo off
rem Paged-KV plan (rev 16.2), step G1, group g1b: a DRAFT build of this group's GPU red cells, modelled on
rem tests\t2791-gpu-prefill-read-red-suite\build_red_suite.bat. The coordinator unifies the groups' drafts.
rem
rem It builds one executable per owning step, each linking only the cells its step can turn green:
rem   g1b_g3.exe  g1b_g3_cells.cpp g1b_g3_pool_ceiling.cpp                      (cells 2.6/G3 2.11/G3 5.7/G3 11.4/G3)
rem   g1b_g4.exe  the G3 files + g1b_g4_cells.cpp                               (+ 2.7/G4 2.11/G4 5.8/G4 9.13/G4)
rem   g1b_g5.exe  the G4 files + g1b_g5_cells.cpp                               (+ 2.6/G5 2.7/G5 2.11/G5 5.9/G5)
rem against the engine compiled with the T-2851 fault seams (SUPERSLM_GPU_ALLOC_FAULT_INJECTION), which 11.4's
rem seam legs need. Expected now (63e7b0e + this suite): every executable RED BY LINK, LNK2019 on exactly the
rem page-pool verbs its cells call (see g1b-notes.md) and, for all three, on the two seams g1b_helpers.h declares
rem (SslmGpuAllocRecordForTest, ArmGpuPoolZeroFillCloseFaultForTest). g1b_g3.exe links once G3 lands, etc.
rem
rem Usage: tests\paged-kv-gpu\g1b_build_draft.bat ["--out=DIR"] ["--shaders=DIR"] ["--gpu-dir=DIR"]
rem   --gpu-dir  a whole copy of src\gpu (every *.cpp in it is compiled): how a mutant engine is built
rem Then run, per GPU, each in its own process (a skipped artifact fails the cell; nothing skips):
rem   <out>\bin\g1b_g3.exe --tiny=<pkv_def.sslm> --m05=<qwen2.5-0.5b.sslm> --m15=<qwen2.5-1.5b.sslm>
rem   <out>\bin\g1b_g3.exe --tiny=<pkv_def.sslm> "=11.4/G3:zerofill"
rem   <out>\bin\g1b_g4.exe --tiny=<pkv_def.sslm> 2.7 2.11/G4 5.8 9.13
rem   <out>\bin\g1b_g5.exe --tiny=<pkv_def.sslm> 2.6/G5 2.7/G5 2.11/G5 5.9
rem pkv_def.sslm comes from: python tools\gen_paged_kv_fixture.py <dir>  (sha256 994b58b4...db1db3)
setlocal enabledelayedexpansion
set HEREDIR=%~dp0
for %%I in ("%HEREDIR%..\..") do set ENG=%%~fI
set OUT=%ENG%\build\g1b
set SHADERS=%ENG%\build\Release\shaders
set GPUDIR=%ENG%\src\gpu
:parse_args
if "%~1"=="" goto :args_done
set ARG=%~1
if "!ARG:~0,6!"=="--out=" set OUT=!ARG:~6!
if "!ARG:~0,10!"=="--shaders=" set SHADERS=!ARG:~10!
if "!ARG:~0,10!"=="--gpu-dir=" set GPUDIR=!ARG:~10!
shift
goto :parse_args
:args_done
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -no_logo
for %%D in (cpu gpu_fi cells bin) do if not exist "%OUT%\%%D" mkdir "%OUT%\%%D"
set CLF=/nologo /std:c++20 /O2 /W4 /fp:precise /EHsc /I"%ENG%\include" /I"%ENG%\tests" /I"%HEREDIR%." /I"%GPUDIR%"
set FI=/DSUPERSLM_GPU_ALLOC_FAULT_INJECTION

echo ===== engine: CPU sources =====
cl /c %CLF% /Fo"%OUT%\cpu\\" "%ENG%\src\artifact.cpp" "%ENG%\src\sha256.cpp" "%ENG%\src\tokenizer.cpp" ^
    "%ENG%\src\model.cpp" "%ENG%\src\intmath.cpp" "%ENG%\src\silu_lut.cpp" "%ENG%\src\matmul.cpp" ^
    "%ENG%\src\proof_manifest.cpp" "%ENG%\src\trace_hook.cpp" "%ENG%\src\forward\checked_chain_funnel.cpp" ^
    "%ENG%\src\forward\forward_sites.cpp" "%ENG%\src\forward\parallel_split.cpp" "%ENG%\src\decode_digest.cpp" > "%OUT%\cpu\build.log" 2>&1
if errorlevel 1 ( type "%OUT%\cpu\build.log" & echo ENGINE CPU BUILD FAILED & exit /b 3 )
call :mklib cpu || exit /b 3

echo ===== engine: GPU sources, T-2851 fault seams compiled in =====
if exist "%OUT%\gpu_fi\*.obj" del /Q "%OUT%\gpu_fi\*.obj"
for %%S in ("%GPUDIR%\*.cpp") do (
    cl /c %CLF% %FI% /Fo"%OUT%\gpu_fi\\" "%%~S" > "%OUT%\gpu_fi\%%~nS.log" 2>&1
    if errorlevel 1 ( type "%OUT%\gpu_fi\%%~nS.log" & echo ENGINE GPU BUILD FAILED: %%~S & exit /b 3 )
)
call :mklib gpu_fi || exit /b 3

if exist "%SHADERS%" (
    if not exist "%OUT%\bin\shaders" mkdir "%OUT%\bin\shaders"
    xcopy /Y /Q "%SHADERS%\*.cso" "%OUT%\bin\shaders\" >nul
) else (
    echo WARNING: no shaders at %SHADERS% -- the GPU cells will fail at context creation
)

echo ===== cells =====
for %%f in (g1b_main_draft g1b_g3_cells g1b_g3_pool_ceiling g1b_g4_cells g1b_g5_cells) do (
    cl /c %CLF% %FI% /Fo"%OUT%\cells\%%f.obj" "%HEREDIR%%%f.cpp" > "%OUT%\cells\%%f.log" 2>&1
    if errorlevel 1 ( type "%OUT%\cells\%%f.log" & echo RED BY COMPILE: %%f -- unexpected & exit /b 2 )
)
set C=%OUT%\cells
call :link g1b_g3 "%C%\g1b_g3_cells.obj" "%C%\g1b_g3_pool_ceiling.obj"
call :link g1b_g4 "%C%\g1b_g3_cells.obj" "%C%\g1b_g3_pool_ceiling.obj" "%C%\g1b_g4_cells.obj"
call :link g1b_g5 "%C%\g1b_g3_cells.obj" "%C%\g1b_g3_pool_ceiling.obj" "%C%\g1b_g4_cells.obj" "%C%\g1b_g5_cells.obj"
exit /b 0

:link
set EXE=%1
shift
set OBJS=
:collect
if "%~1"=="" goto :collected
set OBJS=!OBJS! %1
shift
goto :collect
:collected
link /nologo /OUT:"%OUT%\bin\%EXE%.exe" "%OUT%\cells\g1b_main_draft.obj" %OBJS% "%OUT%\cpu.lib" "%OUT%\gpu_fi.lib" ^
    d3d12.lib dxgi.lib dxguid.lib shlwapi.lib > "%OUT%\cells\%EXE%.link.log" 2>&1
if not errorlevel 1 ( echo %EXE%: LINKED & exit /b 0 )
echo %EXE%: RED BY LINK -- unresolved:
findstr /C:"LNK2019" "%OUT%\cells\%EXE%.link.log"
exit /b 0

:mklib
(for %%o in ("%OUT%\%1\*.obj") do @echo "%%o") > "%OUT%\%1.rsp"
lib /nologo /OUT:"%OUT%\%1.lib" @"%OUT%\%1.rsp" > "%OUT%\%1.lib.log" 2>&1
if errorlevel 1 ( type "%OUT%\%1.lib.log" & echo LIB %1 FAILED & exit /b 3 )
exit /b 0
