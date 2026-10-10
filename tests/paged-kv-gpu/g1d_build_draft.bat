@echo off
rem Paged KV GPU slice, plan step G1 -- DRAFT build of group g1d's cells (3.6e, 3.6f, 3.6g, 3.12, Sec4.4's nine
rem stranded pins). Modelled on tests\t2791-gpu-prefill-read-red-suite\build_red_suite.bat (one GPU engine library
rem per configuration, one cl call per unit, each cell classified LINKED / RED BY LINK / RED BY COMPILE) and on
rem tests\te425-gpu-alloc-status-red-suite\build_red_suite.bat (te425_cells.cpp and te425_getdevice.cpp linked for
rem the hooked cells; here te425_cells.cpp is compiled with /Dmain=te425_cells_main, as carrier row HOOK-legs
rem allows: "a binary that links te425's InstallHooks").
rem
rem Three engine configurations (Sec4.4 *The queue gate*: the gate is in both injection builds):
rem   gpu_alloc  /DSUPERSLM_GPU_ALLOC_FAULT_INJECTION (CMake SUPERSLM_GPU_TEST_SEAMS=ON) -- 3.6e, 3.6f, 3.6g stranded,
rem              the pins
rem   gpu_fi     /DSUPERSLM_T2169_CHUNK_RECORDING_FAULT_INJECTION (t2178's and t2791's build) -- 3.6g recovered
rem   gpu_plain  no definitions -- 3.12
rem Expected before G3 (base 63e7b0e): every cell RED BY LINK, unresolved symbols as each cell's header lists.
rem
rem Usage: tests\paged-kv-gpu\g1d_build_draft.bat ["--out=DIR"] ["--shaders=DIR"] ["--gpu-dir=DIR"]
rem   --out      default <repo>\build\g1d
rem   --shaders  default <repo>\build\Release\shaders
rem   --gpu-dir  a whole copy of src\gpu (a mutant of UploadPoolRegionsSync for 3.12's commissioning runs is built
rem              this way; the cells that include d3d12_harness.h read that copy's header)
rem Then: <out>\bin\g1d_main_draft.exe "--bin=<out>\bin" "--qwen3=..." "--r15=..." "--g5=..." [--step=G3]
setlocal enabledelayedexpansion
set HEREDIR=%~dp0
for %%I in ("%HEREDIR%..\..") do set ENG=%%~fI
set OUT=%ENG%\build\g1d
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
for %%D in (cpu gpu_alloc gpu_fi gpu_plain cells bin) do if not exist "%OUT%\%%D" mkdir "%OUT%\%%D"
set T2791=%ENG%\tests\t2791-gpu-prefill-read-red-suite
set TE425=%ENG%\tests\te425-gpu-alloc-status-red-suite
set CLF=/nologo /std:c++20 /O2 /W4 /fp:precise /EHsc /I"%ENG%\include" /I"%ENG%\src" /I"%GPUDIR%" /I"%ENG%\tests" /I"%HEREDIR%." /I"%T2791%" /I"%TE425%"

echo ===== engine: CPU sources =====
cl /c %CLF% /Fo"%OUT%\cpu\\" "%ENG%\src\artifact.cpp" "%ENG%\src\sha256.cpp" "%ENG%\src\tokenizer.cpp" ^
    "%ENG%\src\model.cpp" "%ENG%\src\intmath.cpp" "%ENG%\src\silu_lut.cpp" "%ENG%\src\matmul.cpp" ^
    "%ENG%\src\proof_manifest.cpp" "%ENG%\src\trace_hook.cpp" "%ENG%\src\forward\checked_chain_funnel.cpp" ^
    "%ENG%\src\forward\forward_sites.cpp" "%ENG%\src\forward\parallel_split.cpp" "%ENG%\src\decode_digest.cpp" > "%OUT%\cpu\build.log" 2>&1
if errorlevel 1 ( type "%OUT%\cpu\build.log" & echo ENGINE CPU BUILD FAILED & exit /b 3 )
call :mklib cpu || exit /b 3
call :gpulib gpu_alloc /DSUPERSLM_GPU_ALLOC_FAULT_INJECTION || exit /b 3
call :gpulib gpu_fi /DSUPERSLM_T2169_CHUNK_RECORDING_FAULT_INJECTION || exit /b 3
call :gpulib gpu_plain || exit /b 3

echo ===== te425 hooks (InstallHooks), main renamed =====
cl /c %CLF% /Dmain=te425_cells_main /Fo"%OUT%\cells\te425_cells.obj" "%TE425%\te425_cells.cpp" > "%OUT%\cells\te425_cells.log" 2>&1 || ( type "%OUT%\cells\te425_cells.log" & exit /b 3 )
cl /c %CLF% /Fo"%OUT%\cells\te425_getdevice.obj" "%TE425%\te425_getdevice.cpp" > "%OUT%\cells\te425_getdevice.log" 2>&1 || ( type "%OUT%\cells\te425_getdevice.log" & exit /b 3 )
set HOOKS="%OUT%\cells\te425_cells.obj" "%OUT%\cells\te425_getdevice.obj" psapi.lib

if exist "%SHADERS%" (
    if not exist "%OUT%\bin\shaders" mkdir "%OUT%\bin\shaders"
    xcopy /Y /Q "%SHADERS%\*.cso" "%OUT%\bin\shaders\" >nul
) else (
    echo WARNING: no shaders at %SHADERS% -- the GPU cells will fail at context creation
)

rem             exe name                  source                       library    defines                                  extra objects
call :buildcell g1d_cell_3_6e_g3          g1d_cell_3_6e                gpu_alloc  "/DSUPERSLM_GPU_ALLOC_FAULT_INJECTION /DG1D_3_6E_G3_ONLY" ""
call :buildcell g1d_cell_3_6e             g1d_cell_3_6e                gpu_alloc  "/DSUPERSLM_GPU_ALLOC_FAULT_INJECTION" ""
call :buildcell g1d_cell_3_6f             g1d_cell_3_6f                gpu_alloc  "/DSUPERSLM_GPU_ALLOC_FAULT_INJECTION" HOOKS
call :buildcell g1d_cell_3_6g_recovered   g1d_cell_3_6g_recovered      gpu_fi     "/DSUPERSLM_T2169_CHUNK_RECORDING_FAULT_INJECTION" ""
call :buildcell g1d_cell_3_6g_stranded    g1d_cell_3_6g_stranded       gpu_alloc  "/DSUPERSLM_GPU_ALLOC_FAULT_INJECTION" HOOKS
call :buildcell g1d_cell_3_12_g4          g1d_cell_3_12                gpu_plain  "/DG1D_3_12_G4_ONLY" ""
call :buildcell g1d_cell_3_12             g1d_cell_3_12                gpu_plain  "" ""
cl %CLF% /Fe"%OUT%\bin\g1d_main_draft.exe" /Fo"%OUT%\cells\g1d_main_draft.obj" "%HEREDIR%g1d_main_draft.cpp" > "%OUT%\cells\g1d_main_draft.log" 2>&1 || type "%OUT%\cells\g1d_main_draft.log"
echo G1D BUILD: done. A cell that did not link is red by the reason printed above.
exit /b 0

:buildcell
set EXE=%1
set SRC=%2
set LIBV=%3
set DEFS=%~4
set EXTRA=
if "%~5"=="HOOKS" set EXTRA=%HOOKS%
echo ===== %EXE% (%SRC%.cpp, %LIBV%) =====
cl /c %CLF% %DEFS% /Fo"%OUT%\cells\%EXE%.obj" "%HEREDIR%%SRC%.cpp" > "%OUT%\cells\%EXE%.log" 2>&1
if errorlevel 1 (
    echo    RED BY COMPILE:
    findstr /R /C:"error C[0-9]*" "%OUT%\cells\%EXE%.log"
    exit /b 0
)
link /nologo /OUT:"%OUT%\bin\%EXE%.exe" "%OUT%\cells\%EXE%.obj" %EXTRA% "%OUT%\cpu.lib" "%OUT%\%LIBV%.lib" ^
    d3d12.lib dxgi.lib dxguid.lib >> "%OUT%\cells\%EXE%.log" 2>&1
if not errorlevel 1 ( echo    LINKED & exit /b 0 )
findstr /C:"LNK2019" "%OUT%\cells\%EXE%.log" >nul
if not errorlevel 1 (
    echo    RED BY LINK -- unresolved:
    findstr /C:"LNK2019" "%OUT%\cells\%EXE%.log"
) else (
    echo    UNEXPECTED LINK FAILURE -- see %OUT%\cells\%EXE%.log
)
exit /b 0

:gpulib
echo ===== engine: GPU sources, %1 %2 =====
if exist "%OUT%\%1\*.obj" del /Q "%OUT%\%1\*.obj"
for %%S in ("%GPUDIR%\superslm_gpu.cpp" "%GPUDIR%\gpu_1p0.cpp") do (
    cl /c %CLF% %2 /Fo"%OUT%\%1\\" "%%~S" > "%OUT%\%1\%%~nS.log" 2>&1
    if errorlevel 1 ( type "%OUT%\%1\%%~nS.log" & echo ENGINE GPU BUILD FAILED: %%~S & exit /b 3 )
)
call :mklib %1 || exit /b 3
exit /b 0

:mklib
(for %%o in ("%OUT%\%1\*.obj") do @echo "%%o") > "%OUT%\%1.rsp"
lib /nologo /OUT:"%OUT%\%1.lib" @"%OUT%\%1.rsp" > "%OUT%\%1.lib.log" 2>&1
if errorlevel 1 ( type "%OUT%\%1.lib.log" & echo LIB %1 FAILED & exit /b 3 )
exit /b 0
