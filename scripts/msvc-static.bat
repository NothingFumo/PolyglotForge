@echo off
cd /d "%~dp0.."
if not exist out mkdir out
set PYEXE=python
%PYEXE% -c "pass" >nul 2>nul || set PYEXE=D:\Python\Python311\python.exe
"%PYEXE%" scripts\env.py --bat > out\env.bat || exit /b 1
call out\env.bat
call "%QF_VCVARS%" >nul || (echo [mt] vcvars64.bat 调用失败 & exit /b 1)
set PYEXE=%QF_PYEXE%
set AOT=%QF_AOT%
set MINGW=%QF_STRAWBERRY%\x86_64-w64-mingw32\lib
set STDCXX=%QF_STRAWBERRY%\lib\libstdc++.a
set GCCLIB=%QF_GCCLIB%
set CLANG=%QF_LLVM%\clang.exe
set LLVMAR=%QF_LLVM%\llvm-ar.exe

rem MinGW/MSVC 混合链接所需的归档，独立于 out\msvc 的其它内容重新归一化。
if not exist out\msvc\mingw mkdir out\msvc\mingw
if not exist out\msvc\mingw\libstdcxx.a "%PYEXE%" stages\stubs\coff_msvc_fix.py "%STDCXX%" out\msvc\mingw\libstdcxx.a || exit /b 1
if not exist out\msvc\mingw\libgcc_eh.a "%PYEXE%" stages\stubs\coff_msvc_fix.py "%GCCLIB%\libgcc_eh.a" out\msvc\mingw\libgcc_eh.a || exit /b 1
if not exist out\msvc\mingw\libwinpthread.a "%PYEXE%" stages\stubs\coff_msvc_fix.py "%MINGW%\libwinpthread.a" out\msvc\mingw\libwinpthread.a || exit /b 1
if not exist out\msvc\mingw\libcxxabi_shim.a (
  "%CLANG%" --target=x86_64-w64-mingw32 -c -O2 -fexceptions stages\stubs\cxxabi_shim.cpp -o out\msvc\cxxabi_shim.o || exit /b 1
  "%LLVMAR%" rcs out\msvc\cxxabi_shim.a out\msvc\cxxabi_shim.o || exit /b 1
  "%PYEXE%" stages\stubs\coff_msvc_fix.py out\msvc\cxxabi_shim.a out\msvc\mingw\libcxxabi_shim.a || exit /b 1
)

rem 阶段清单：defines 与阶段归档的唯一来源（scripts/stages.py）。
"%PYEXE%" scripts\stages.py --bat msvc-static > out\stages-msvc-static.bat || exit /b 1
call out\stages-msvc-static.bat
set DEFS=%QF_DEFS% /DQF_HAVE_PY_PAYLOAD
if not exist out\msvc mkdir out\msvc
if not exist out\mt mkdir out\mt
cl /nologo /c /O2 /MT /std:c11 %DEFS% /Igen /Icontracts /Foout\mt\ stages\c\main.c stages\c\arena.c stages\c\host.c stages\c\engine.c stages\stubs\stubs.c stages\stubs\mingw_compat.c stages\stubs\go_crt_init.c
if errorlevel 1 exit /b 1
cl /nologo /c /O2 /MT /std:c++17 /EHsc /Igen /Icontracts /Foout\mt\ stages\cpp\parser.cpp
if errorlevel 1 exit /b 1
rem 兼容垫片由本脚本自己编译（以前依赖手工留下的 out\mt\mingw_crt_shim.obj）。
cl /nologo /c /O2 /MT /Foout\mt\mingw_crt_shim.obj stages\stubs\mingw_crt_shim.c
if errorlevel 1 exit /b 1
for %%a in (%QF_STAGE_ARCHS%) do if not exist "%%a" (echo [mt] 缺少阶段归档 %%a & exit /b 1)

set NODEF=/NODEFAULTLIB:msvcrt.lib /NODEFAULTLIB:msvcrtd.lib /NODEFAULTLIB:vcruntime.lib /NODEFAULTLIB:vcruntimed.lib /NODEFAULTLIB:ucrt.lib /NODEFAULTLIB:ucrtd.lib /NODEFAULTLIB:libstdc++.a /NODEFAULTLIB:libgcc.a /NODEFAULTLIB:libgcc_eh.a /NODEFAULTLIB:libmingw32.a
link /nologo /FORCE:MULTIPLE /OUT:dist\queryforge-static-msvc.exe /SUBSYSTEM:CONSOLE %QF_STAGE_ARCHS% %NODEF% out\mt\main.obj out\mt\arena.obj out\mt\host.obj out\mt\engine.obj out\mt\stubs.obj out\mt\parser.obj out\mt\mingw_compat.obj out\mt\go_crt_init.obj out\mt\mingw_crt_shim.obj out\msvc\mingw\libcxxabi_shim.a out\msvc\mingw\libstdcxx.a out\msvc\mingw\libgcc_eh.a out\msvc\mingw\libwinpthread.a "%QF_KONAN_GCC%" "%AOT%\bootstrapperdll.obj" "%AOT%\Runtime.WorkstationGC.lib" "%AOT%\Runtime.VxsortEnabled.lib" "%AOT%\eventpipe-disabled.lib" "%AOT%\standalonegc-disabled.lib" "%AOT%\aotminipal.lib" "%AOT%\System.IO.Compression.Native.Aot.lib" "%AOT%\System.Globalization.Native.Aot.lib" "%AOT%\zlibstatic.lib" "%AOT%\brotlienc.lib" "%AOT%\brotlidec.lib" "%AOT%\brotlicommon.lib" stages\python\out\python311.lib libcmt.lib libvcruntime.lib libucrt.lib kernel32.lib user32.lib advapi32.lib ws2_32.lib ole32.lib oleaut32.lib uuid.lib pathcch.lib version.lib synchronization.lib userenv.lib bcrypt.lib ntdll.lib
if errorlevel 1 exit /b 1
echo [mt] ok
