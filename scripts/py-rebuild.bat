@echo off
rem 重编 CPython 静态库：/MT + 已冻结 encodings（见 ADR 0008）。
set ROOT=%~dp0..
if not exist "%ROOT%\out" mkdir "%ROOT%\out"
python "%ROOT%\scripts\env.py" --bat > "%ROOT%\out\env.bat" || exit /b 1
call "%ROOT%\out\env.bat"
call "%QF_VCVARS%" >nul || exit /b 1
cd /d "%ROOT%\stages\python\vendor\Python-3.11.9"
msbuild PCbuild\pythoncore.vcxproj /nologo /v:m /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:ConfigurationType=StaticLibrary /p:RuntimeLibrary=MultiThreaded
if errorlevel 1 exit /b 1
copy /y PCbuild\amd64\python311.lib "%ROOT%\stages\python\out\python311.lib"
if errorlevel 1 exit /b 1
echo [py] rebuilt
