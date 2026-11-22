#!/usr/bin/env bash
# QuickJS 的 MSVC 构建：cl 下 _WIN32 分支自带 Windows 原语，
# 不再需要 winpthreads，因此可被 link.exe 直接消费。
set -euo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/env.sh"
cd "$(dirname "$0")/../.."
root="$PWD"

# command -v cmd 返回 "C:\Windows\system32/cmd.exe"（混合分隔符），作为 argv[0]
# 交给 CreateProcess 会丢工作目录；优先用短名，让 shell 自己解析。
if command -v cmd >/dev/null 2>&1; then
    CMD=cmd
else
    CMD="$(qf_require /mnt/c/Windows/System32/cmd.exe /c/Windows/System32/cmd.exe)" \
        || { echo "找不到 cmd" >&2; exit 1; }
fi

QJ="stages/js/vendor/quickjs"
OUT="stages/js/out"
OBJ="$OUT/msvc"
[ -f "$QJ/quickjs.c" ] || { echo "缺少 vendored QuickJS: $QJ" >&2; exit 1; }
mkdir -p "$OBJ"

QJS_VER="$(cat "$QJ/VERSION")"
BAT="$OUT/build-quickjs-msvc.bat"
root_win="$(printf '%s' "$root" | sed 's|^/mnt/\([a-z]\)/|\U\1:/|; s|^/\([a-z]\)/|\U\1:/|')"

# 工具位置来自 env.py；clang-cl 才能吃 quickjs.c 的 GNU 扩展（cl 会报 C2059）。
python scripts/env.py --bat > out/env.bat || { echo "env.py 失败" >&2; exit 1; }

# 用 CRLF 写入批处理（cmd.exe 不认 LF-only 脚本）。
{
    printf '@echo off\r\n'
    printf 'call out\\env.bat\r\n'
    printf 'call "%%QF_VCVARS%%" >nul || exit /b 1\r\n'
    printf 'cd /d %s\r\n' "$root_win"
    printf 'set QJ=stages\\js\\vendor\\quickjs\r\n'
    printf 'set OUT=stages\\js\\out\\msvc\r\n'
    printf 'set CC="%%QF_LLVM%%\\clang-cl.exe"\r\n'
    printf 'set DEFS=/Dalloca=_alloca /D__STDC_NO_ATOMICS__ /DCONFIG_VERSION=\\"%s\\"\r\n' "$QJS_VER"
    printf 'del /q %%OUT%%\\*.obj 2>nul\r\n'
    printf 'for %%%%f in (quickjs cutils dtoa libregexp libunicode) do %%CC%% /nologo /c /O2 /MT /std:c11 -Wno-everything %%DEFS%% /I%%QJ%% /Istages/js/vendor/msvc_stubs /Fo%%OUT%%\\%%%%f.obj %%QJ%%\\%%%%f.c || exit /b 1\r\n'
    printf '%%CC%% /nologo /c /O2 /MT /std:c11 -Wno-everything %%DEFS%% /Igen /Icontracts /Istages\\js /I%%QJ%% /Istages/js/vendor/msvc_stubs /Fo%%OUT%%\\qf_js.obj stages\\js\\qf_js.c || exit /b 1\r\n'
    printf 'lib /nologo /OUT:stages\\js\\out\\qf_js_msvc.lib %%OUT%%\\*.obj || exit /b 1\r\n'
    printf 'echo [js-msvc] ok\r\n'
} > "$BAT"

MSYS_NO_PATHCONV=1 "$CMD" /c "stages\\js\\out\\build-quickjs-msvc.bat" 2>&1 | tail -12
[ -f stages/js/out/qf_js_msvc.lib ] || { echo "[js-msvc] 库未生成" >&2; exit 1; }
