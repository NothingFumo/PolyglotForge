#!/usr/bin/env bash
# QuickJS 的 MSVC 构建：cl 下 _WIN32 分支自带 Windows 原语，
# 不再需要 winpthreads，因此可被 link.exe 直接消费。
set -euo pipefail
cd "$(dirname "$0")/../.."
root="$PWD"

pick() {
    for c in "$@"; do
        if [ -n "$c" ] && [ -x "$c" ]; then printf '%s' "$c"; return 0; fi
    done
    return 1
}
CMD="$(pick "$(command -v cmd 2>/dev/null || true)" /mnt/c/Windows/System32/cmd.exe /c/Windows/System32/cmd.exe)" \
    || { echo "找不到 cmd" >&2; exit 1; }

QJ="stages/js/vendor/quickjs"
OUT="stages/js/out"
OBJ="$OUT/msvc"
[ -f "$QJ/quickjs.c" ] || { echo "缺少 vendored QuickJS: $QJ" >&2; exit 1; }
mkdir -p "$OBJ"

QJS_VER="$(cat "$QJ/VERSION")"
BAT="$OUT/build-quickjs-msvc.bat"

# 用 CRLF 写入批处理（cmd.exe 不认 LF-only 脚本）。
{
    printf '@echo off\r\n'
    printf 'call "D:\\Microsoft Visual Studio\\18\\Enterprise\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\r\n'
    printf 'cd /d %s\r\n' "$(printf '%s' "$root" | sed 's|^/mnt/\([a-z]\)/|\U\1:/|; s|^/\([a-z]\)/|\U\1:/|')"
    printf 'set QJ=stages\\js\\vendor\\quickjs\r\n'
    printf 'set OUT=stages\\js\\out\\msvc\r\n'
    printf 'set DEFS=/D_GNU_SOURCE /DCONFIG_VERSION=\\"%s\\"\r\n' "$QJS_VER"
    printf 'del /q %%OUT%%\\*.obj 2>nul\r\n'
    for f in quickjs cutils dtoa libregexp libunicode; do
        printf 'cl /nologo /c /O2 /std:c11 %%DEFS%% /I%%QJ%% /Fo%%OUT%%\\%s.obj %%QJ%%\\%s.c\r\n' "$f" "$f"
    done
    printf 'cl /nologo /c /O2 /std:c11 /Igen /Icontracts /Istages\\js /I%%QJ%% %%DEFS%% /Fo%%OUT%%\\qf_js.obj stages\\js\\qf_js.c\r\n'
    printf 'if errorlevel 1 exit /b 1\r\n'
    printf 'lib /nologo /OUT:stages\\js\\out\\qf_js_msvc.lib %%OUT%%\\*.obj\r\n'
    printf 'if errorlevel 1 exit /b 1\r\n'
    printf 'echo [js-msvc] ok\r\n'
} > "$BAT"

"$CMD" /c "stages\\js\\out\\build-quickjs-msvc.bat" 2>&1 | tail -12
