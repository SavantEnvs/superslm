@echo off
rem Paged-KV plan (rev 16.2) step G1, group g1e: DRAFT build and run of the g1e cells (9.6, 9.8 and its
rem old-reader leg, 9.12, 9.13's G5 rows, 8.17). Modelled on tests\t2791-gpu-prefill-read-red-suite\
rem build_red_suite.bat; the coordinator folds it into the unified tests\paged-kv-gpu runner.
rem
rem Differences from t2791's script, and why:
rem   * the CPU side links the CMake `superslm` static library (--cpu-lib, default
rem     build\Release\superslm.lib), not t2791's hand-built cpu.lib: g1e_cell_8_17 drives the CPU C ABI
rem     (src\sslm_abi.cpp), which t2791's source list leaves out. Every g1e cell links the same library.
rem   * the old reader of cell 9.8 (g1e_old_reader_9_8.cpp) is compiled against ANOTHER tree, --old-tree,
rem     the v1.12.0 checkout (the GPU slice's base, the tree before G5), with that tree's own CPU library
rem     (--old-cpu-lib) and GPU sources, so it is the v1.12.0 reader.
rem
rem Expected now (G1): every g1e cell RED BY LINK on the G3-G5 verbs (LNK2019: sslm_gpu_kv_pool_create,
rem sslm_gpu_kv_pool_destroy, sslm_gpu_seq_create_budgeted, sslm_gpu_prefix_*, sslm_gpu_seq_adopt_prefix,
rem sslm_gpu_seq_restore_into_pool, sslm_gpu_test_only_peek_pool_mirror_bytes,
rem sslm_gpu_test_only_wait_in_flight); the old reader LINKS against --old-tree and fails at run for
rem want of leg 1's files.
rem
rem Usage: g1e_build_draft.bat ["--out=DIR"] ["--shaders=DIR"] ["--cpu-lib=FILE"] ["--old-tree=DIR"] ["--old-cpu-lib=FILE"]
rem Then, on each GPU (SSLM_GPU_ADAPTER_INDEX=0 and =1) with SSLM_GPU_ENABLE_DEBUG_LAYER=1:
rem   python tools\gen_paged_kv_fixture.py D:\_scratch\pkv-fix
rem   for %c in (9_6 9_12 9_13 8_17) do %OUT%\bin\g1e_cell_%c.exe --fixture-dir=D:\_scratch\pkv-fix
rem   set SUPERSLM_PKV_GPU_98_OUT=D:\_scratch\pkv-gpu-98 & %OUT%\bin\g1e_cell_9_8.exe --fixture-dir=D:\_scratch\pkv-fix
rem   %OUT%\old\g1e_old_reader_9_8.exe --model=D:\_scratch\pkv-fix\pkv_def.sslm --in=D:\_scratch\pkv-gpu-98
setlocal enabledelayedexpansion
set HEREDIR=%~dp0
for %%I in ("%HEREDIR%..\..") do set ENG=%%~fI
set OUT=%ENG%\build\pkv-gpu-g1e
set SHADERS=%ENG%\build\Release\shaders
set CPULIB=%ENG%\build\Release\superslm.lib
set OLDTREE=
set OLDCPULIB=
:parse_args
if "%~1"=="" goto :args_done
set ARG=%~1
if "!ARG:~0,6!"=="--out=" set OUT=!ARG:~6!
if "!ARG:~0,10!"=="--shaders=" set SHADERS=!ARG:~10!
if "!ARG:~0,10!"=="--cpu-lib=" set CPULIB=!ARG:~10!
if "!ARG:~0,11!"=="--old-tree=" set OLDTREE=!ARG:~11!
if "!ARG:~0,14!"=="--old-cpu-lib=" set OLDCPULIB=!ARG:~14!
shift
goto :parse_args
:args_done
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -no_logo
for %%D in (gpu cells bin old) do if not exist "%OUT%\%%D" mkdir "%OUT%\%%D"
if not exist "%CPULIB%" ( echo NO CPU LIBRARY AT %CPULIB% -- build the CMake superslm target first & exit /b 3 )
set CLF=/nologo /std:c++20 /O2 /W4 /fp:precise /EHsc /I"%ENG%\include" /I"%ENG%\tests" /I"%ENG%\tests\t2791-gpu-prefill-read-red-suite" /I"%HEREDIR%."

echo ===== engine: GPU sources (this tree) =====
for %%S in ("%ENG%\src\gpu\superslm_gpu.cpp" "%ENG%\src\gpu\gpu_1p0.cpp") do (
    cl /c %CLF% /I"%ENG%\src\gpu" /Fo"%OUT%\gpu\\" "%%~S" > "%OUT%\gpu\%%~nS.log" 2>&1
    if errorlevel 1 ( type "%OUT%\gpu\%%~nS.log" & echo ENGINE GPU BUILD FAILED & exit /b 3 )
)
lib /nologo /OUT:"%OUT%\gpu.lib" "%OUT%\gpu\superslm_gpu.obj" "%OUT%\gpu\gpu_1p0.obj" || exit /b 3
if exist "%SHADERS%" (
    if not exist "%OUT%\bin\shaders" mkdir "%OUT%\bin\shaders"
    xcopy /Y /Q "%SHADERS%\*.cso" "%OUT%\bin\shaders\" >nul
) else (
    echo WARNING: no shaders at %SHADERS% -- the cells will fail at context creation
)
for %%f in (g1e_cell_9_6 g1e_cell_9_8 g1e_cell_9_12 g1e_cell_9_13 g1e_cell_8_17) do call :buildcell %%f
if defined OLDTREE call :oldreader
exit /b 0

:buildcell
echo ===== %1 =====
cl /c %CLF% /Fo"%OUT%\cells\%1.obj" "%HEREDIR%%1.cpp" > "%OUT%\cells\%1.log" 2>&1
if errorlevel 1 ( echo    RED BY COMPILE: & findstr /R /C:"error C[0-9]*" "%OUT%\cells\%1.log" & exit /b 0 )
link /nologo /OUT:"%OUT%\bin\%1.exe" "%OUT%\cells\%1.obj" "%CPULIB%" "%OUT%\gpu.lib" d3d12.lib dxgi.lib dxguid.lib >> "%OUT%\cells\%1.log" 2>&1
if not errorlevel 1 ( echo    LINKED -- run it: %OUT%\bin\%1.exe --fixture-dir=DIR & exit /b 0 )
echo    RED BY LINK -- unresolved:
findstr /C:"LNK2019" "%OUT%\cells\%1.log"
exit /b 0

rem Cell 9.8's leg 2, against the v1.12.0 tree: its own GPU sources, its own CPU library.
:oldreader
echo ===== g1e_old_reader_9_8 against %OLDTREE% =====
if not defined OLDCPULIB set OLDCPULIB=%OLDTREE%\build\Release\superslm.lib
if not exist "%OLDCPULIB%" ( echo NO v1.12.0 CPU LIBRARY AT %OLDCPULIB% & exit /b 3 )
set OCLF=/nologo /std:c++20 /O2 /W4 /fp:precise /EHsc /I"%OLDTREE%\include"
for %%S in ("%OLDTREE%\src\gpu\superslm_gpu.cpp" "%OLDTREE%\src\gpu\gpu_1p0.cpp") do (
    cl /c %OCLF% /I"%OLDTREE%\src\gpu" /Fo"%OUT%\old\\" "%%~S" > "%OUT%\old\%%~nS.log" 2>&1
    if errorlevel 1 ( type "%OUT%\old\%%~nS.log" & echo v1.12.0 GPU BUILD FAILED & exit /b 3 )
)
cl /c %OCLF% /Fo"%OUT%\old\g1e_old_reader_9_8.obj" "%HEREDIR%g1e_old_reader_9_8.cpp" > "%OUT%\old\reader.log" 2>&1 || ( type "%OUT%\old\reader.log" & exit /b 3 )
link /nologo /OUT:"%OUT%\old\g1e_old_reader_9_8.exe" "%OUT%\old\g1e_old_reader_9_8.obj" "%OUT%\old\superslm_gpu.obj" "%OUT%\old\gpu_1p0.obj" ^
    "%OLDCPULIB%" d3d12.lib dxgi.lib dxguid.lib >> "%OUT%\old\reader.log" 2>&1 || ( type "%OUT%\old\reader.log" & exit /b 3 )
if exist "%SHADERS%" (
    if not exist "%OUT%\old\shaders" mkdir "%OUT%\old\shaders"
    xcopy /Y /Q "%OLDTREE%\build\Release\shaders\*.cso" "%OUT%\old\shaders\" >nul
)
echo    LINKED (v1.12.0): %OUT%\old\g1e_old_reader_9_8.exe
exit /b 0
