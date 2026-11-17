@echo off
call "D:\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d D:\workspace\PolyglotForge
set AOT=C:\Users\Lenovo\.nuget\packages\microsoft.netcore.app.runtime.nativeaot.win-x64\10.0.11\runtimes\win-x64\native
set DEFS=/DQF_HAVE_CPP /DQF_HAVE_RUST /DQF_HAVE_CSHARP /DQF_HAVE_KOTLIN /DQF_HAVE_PYTHON /DQF_HAVE_GO /DQF_HAVE_JAVASCRIPT /DQF_HAVE_PY_PAYLOAD
if not exist out\mt mkdir out\mt
cl /nologo /c /O2 /MT /std:c11 %DEFS% /Igen /Icontracts /Foout\mt\ stages\c\main.c stages\c\arena.c stages\c\host.c stages\c\engine.c stages\stubs\stubs.c stages\stubs\mingw_compat.c stages\stubs\go_crt_init.c
if errorlevel 1 exit /b 1
cl /nologo /c /O2 /MT /std:c++17 /EHsc /Igen /Icontracts /Foout\mt\ stages\cpp\parser.cpp
if errorlevel 1 exit /b 1
set NODEF=/NODEFAULTLIB:msvcrt.lib /NODEFAULTLIB:msvcrtd.lib /NODEFAULTLIB:vcruntime.lib /NODEFAULTLIB:vcruntimed.lib /NODEFAULTLIB:ucrt.lib /NODEFAULTLIB:ucrtd.lib /NODEFAULTLIB:libstdc++.a /NODEFAULTLIB:libgcc.a /NODEFAULTLIB:libgcc_eh.a /NODEFAULTLIB:libmingw32.a
link /nologo /FORCE:MULTIPLE /OUT:dist\queryforge-static-msvc.exe /SUBSYSTEM:CONSOLE %NODEF% out\mt\main.obj out\mt\arena.obj out\mt\host.obj out\mt\engine.obj out\mt\stubs.obj out\mt\parser.obj out\mt\mingw_compat.obj out\mt\go_crt_init.obj out\mt\mingw_crt_shim.obj stages\rust-crt-static.lib stages\csharp\out-mt\qf_cs.lib out\msvc\mingw\libqf_kt.a out\msvc\mingw\libcxxabi_shim.a out\msvc\mingw\libstdcxx.a out\msvc\mingw\libgcc_eh.a out\msvc\mingw\libwinpthread.a "C:\Users\Lenovo\.konan\dependencies\msys2-mingw-w64-x86_64-2\lib\gcc\x86_64-w64-mingw32\9.2.0\libgcc.a" "%AOT%\bootstrapperdll.obj" "%AOT%\Runtime.WorkstationGC.lib" "%AOT%\Runtime.VxsortEnabled.lib" "%AOT%\eventpipe-disabled.lib" "%AOT%\standalonegc-disabled.lib" "%AOT%\aotminipal.lib" "%AOT%\System.IO.Compression.Native.Aot.lib" "%AOT%\System.Globalization.Native.Aot.lib" "%AOT%\zlibstatic.lib" "%AOT%\brotlienc.lib" "%AOT%\brotlidec.lib" "%AOT%\brotlicommon.lib" stages\go\out\libqf_go.a stages\js\out\qf_js_msvc.lib stages\python\out\qf_py.obj out\payload_blob.obj stages\python\out\python311.lib libcmt.lib libvcruntime.lib libucrt.lib kernel32.lib user32.lib advapi32.lib ws2_32.lib ole32.lib oleaut32.lib uuid.lib pathcch.lib version.lib synchronization.lib userenv.lib bcrypt.lib ntdll.lib
if errorlevel 1 exit /b 1
echo [mt] ok
