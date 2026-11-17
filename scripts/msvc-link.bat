@echo off
call "D:\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d D:\workspace\PolyglotForge
set AOT=C:\Users\Lenovo\.nuget\packages\microsoft.netcore.app.runtime.nativeaot.win-x64\10.0.11\runtimes\win-x64\native
link /nologo /OUT:dist\queryforge-msvc.exe /SUBSYSTEM:CONSOLE out\msvc\main.obj out\msvc\arena.obj out\msvc\host.obj out\msvc\engine.obj out\msvc\stubs.obj out\msvc\parser.obj stages\rust\target\x86_64-pc-windows-msvc\release\qf_rust.lib stages\csharp\out\qf_cs.lib "%AOT%\bootstrapperdll.obj" "%AOT%\Runtime.WorkstationGC.lib" "%AOT%\Runtime.VxsortEnabled.lib" "%AOT%\eventpipe-disabled.lib" "%AOT%\standalonegc-disabled.lib" "%AOT%\aotminipal.lib" "%AOT%\System.IO.Compression.Native.Aot.lib" "%AOT%\System.Globalization.Native.Aot.lib" "%AOT%\zlibstatic.lib" "%AOT%\brotlienc.lib" "%AOT%\brotlidec.lib" "%AOT%\brotlicommon.lib" stages\python\out\qf_py.obj stages\python\out\python311.lib kernel32.lib user32.lib advapi32.lib ws2_32.lib ole32.lib oleaut32.lib uuid.lib bcrypt.lib ntdll.lib msvcrt.lib legacy_stdio_definitions.lib version.lib pathcch.lib synchronization.lib userenv.lib
if errorlevel 1 exit /b 1
echo [msvc] ok
