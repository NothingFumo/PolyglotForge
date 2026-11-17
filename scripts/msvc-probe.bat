@echo off
call "D:\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d D:\workspace\PolyglotForge
if not exist out\msvc mkdir out\msvc
set AOT=C:\Users\Lenovo\.nuget\packages\microsoft.netcore.app.runtime.nativeaot.win-x64\10.0.11\runtimes\win-x64\native
cl /nologo /c /O2 /std:c11 /DQF_HAVE_CPP /DQF_HAVE_RUST /DQF_HAVE_CSHARP /DQF_HAVE_PYTHON /Igen /Icontracts /Foout\msvc\ stages\c\main.c stages\c\arena.c stages\c\host.c stages\c\engine.c stages\stubs\stubs.c
if errorlevel 1 exit /b 1
cl /nologo /c /O2 /std:c++17 /EHsc /Igen /Icontracts /Foout\msvc\ stages\cpp\parser.cpp
if errorlevel 1 exit /b 1
link /nologo /OUT:dist\queryforge-msvc.exe /SUBSYSTEM:CONSOLE out\msvc\main.obj out\msvc\arena.obj out\msvc\host.obj out\msvc\engine.obj out\msvc\stubs.obj out\msvc\parser.obj stages\rust\target\x86_64-pc-windows-msvc\release\qf_rust.lib stages\csharp\out\qf_cs.lib "%AOT%\bootstrapperdll.obj" "%AOT%\Runtime.WorkstationGC.lib" "%AOT%\aotminipal.lib" stages\python\out\qf_py.obj stages\python\out\python311.lib kernel32.lib user32.lib advapi32.lib ws2_32.lib ole32.lib oleaut32.lib uuid.lib bcrypt.lib ntdll.lib
if errorlevel 1 exit /b 1
echo [msvc] ok
