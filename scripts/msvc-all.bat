@echo off
rem MSVC 装配器：用一个 link.exe 把九个阶段的产物装进同一个 exe。
rem
rem 与 MinGW 主干的分工见 docs/adr/0003、0007：C/C++/Rust 用 MSVC ABI 编译，
rem C#（NativeAOT）、CPython、Kotlin/Native、Go（c-archive）、QuickJS 用各自工具链
rem 产出静态库/对象，最终由 link.exe 统一装配。三条实测出来的 link.exe 契约由
rem stages\stubs 承担：
rem   - go_crt_init.c            Go 把运行时初始化注册在 .ctors，link.exe 不认 -> 补 .CRT$XCU
rem   - coff_msvc_fix.py         GNU 产物的 .ctors/COMDAT/长节名改成 link.exe 能吃的形状
rem   - mingw_compat.c           MinGW 运行时里 link.exe 侧缺失的两个符号
setlocal
call "D:\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d D:\workspace\PolyglotForge

set AOT=C:\Users\Lenovo\.nuget\packages\microsoft.netcore.app.runtime.nativeaot.win-x64\10.0.11\runtimes\win-x64\native
set MINGW=C:\Strawberry\c\x86_64-w64-mingw32\lib
set GCCLIB=C:\Strawberry\c\lib\gcc\x86_64-w64-mingw32\13.2.0\libgcc.a
set GCCEH=C:\Strawberry\c\lib\gcc\x86_64-w64-mingw32\13.2.0\libgcc_eh.a
set CXXLIB=C:\Strawberry\c\lib\libstdc++.a

set PYEXE=python
%PYEXE% -c "pass" >nul 2>nul
if errorlevel 1 set PYEXE=D:\Python\Python311\python.exe
%PYEXE% -c "pass" >nul 2>nul
if errorlevel 1 (
    echo [msvc] 需要 python 来把 GNU COFF 改写成 link.exe 的形状
    exit /b 1
)

if not exist out\msvc mkdir out\msvc
if not exist out\msvc\mingw mkdir out\msvc\mingw

echo [msvc] 编译 C 与兼容层
cl /nologo /c /O2 /std:c11 /DQF_HAVE_CPP /DQF_HAVE_RUST /DQF_HAVE_CSHARP /DQF_HAVE_KOTLIN /DQF_HAVE_GO /DQF_HAVE_PYTHON /DQF_HAVE_JAVASCRIPT /Igen /Icontracts /Foout\msvc\ stages\c\main.c stages\c\arena.c stages\c\host.c stages\c\engine.c stages\stubs\stubs.c stages\stubs\mingw_compat.c stages\stubs\go_crt_init.c
if errorlevel 1 exit /b 1
echo [msvc] 编译 C++ 解析器
cl /nologo /c /O2 /std:c++17 /EHsc /Igen /Icontracts /Foout\msvc\ stages\cpp\parser.cpp
if errorlevel 1 exit /b 1

echo [msvc] 准备 Python 标准库载荷
if not exist out\payload_blob.obj pushd out && "%PYEXE%" ..\scripts\make_payload.py && C:\Strawberry\c\bin\ld.exe -r -b binary payload\lib.zip -o payload_blob.obj && popd
if not exist out\payload_blob.obj (echo [msvc] 载荷对象生成失败 & exit /b 1)

echo [msvc] 改写 GNU COFF（.ctors 改成 .CRT$XCU、COMDAT 关联化、长节名折叠）
"%PYEXE%" stages\stubs\coff_msvc_fix.py stages\kotlin\build\libqf_kt.a out\msvc\mingw\libqf_kt.a || exit /b 1
"%PYEXE%" stages\stubs\coff_msvc_fix.py "%CXXLIB%" out\msvc\mingw\libstdc++.a || exit /b 1
"%PYEXE%" stages\stubs\coff_msvc_fix.py "%MINGW%\libmingwex.a" out\msvc\mingw\libmingwex.a || exit /b 1
"%PYEXE%" stages\stubs\coff_msvc_fix.py "%MINGW%\libwinpthread.a" out\msvc\mingw\libwinpthread.a || exit /b 1
"%PYEXE%" stages\stubs\coff_msvc_fix.py "%GCCLIB%" out\msvc\mingw\libgcc.a || exit /b 1
"%PYEXE%" stages\stubs\coff_msvc_fix.py "%GCCEH%" out\msvc\mingw\libgcc_eh.a || exit /b 1

echo [msvc] 链接
rem /FORCE:MULTIPLE：mingwex 与 ucrt 都提供 ldexp/modf/round 等 C99 数学函数，
rem 以及 __guard_check_icall_fptr 这类 CFG 桩。两套 CRT 混装时这是必然重叠，
rem 语义一致（都是 ucrt 的转发），实测只影响 warning 而不影响行为。
link /nologo /FORCE:MULTIPLE /OUT:dist\queryforge-all.exe /SUBSYSTEM:CONSOLE out\msvc\main.obj out\msvc\arena.obj out\msvc\host.obj out\msvc\engine.obj out\msvc\stubs.obj out\payload_blob.obj out\msvc\parser.obj out\msvc\mingw_compat.obj out\msvc\go_crt_init.obj stages\rust\target\x86_64-pc-windows-msvc\release\qf_rust.lib stages\csharp\out\qf_cs.lib out\msvc\mingw\libqf_kt.a "%AOT%\bootstrapperdll.obj" "%AOT%\Runtime.WorkstationGC.lib" "%AOT%\Runtime.VxsortEnabled.lib" "%AOT%\eventpipe-disabled.lib" "%AOT%\standalonegc-disabled.lib" "%AOT%\aotminipal.lib" "%AOT%\System.IO.Compression.Native.Aot.lib" "%AOT%\System.Globalization.Native.Aot.lib" "%AOT%\zlibstatic.lib" "%AOT%\brotlienc.lib" "%AOT%\brotlidec.lib" "%AOT%\brotlicommon.lib" stages\go\out\libqf_go.a stages\js\out\libqf_js.a out\msvc\mingw\libstdc++.a out\msvc\mingw\libmingwex.a out\msvc\mingw\libwinpthread.a out\msvc\mingw\libgcc.a out\msvc\mingw\libgcc_eh.a stages\python\out\qf_py.obj stages\python\out\python311.lib kernel32.lib user32.lib advapi32.lib ws2_32.lib ole32.lib oleaut32.lib uuid.lib bcrypt.lib ntdll.lib msvcrt.lib legacy_stdio_definitions.lib version.lib pathcch.lib synchronization.lib userenv.lib
if errorlevel 1 exit /b 1
echo [msvc] ok: dist\queryforge-all.exe
