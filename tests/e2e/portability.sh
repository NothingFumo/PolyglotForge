#!/usr/bin/env bash
# 产物可移植性：干净 PATH 下逐个启动；动态产物必须随包第三方 DLL，
# 静态产物不得引用任何非系统 DLL；Python UDF 必须零落盘。
set -uo pipefail
. "$(cd "$(dirname "$0")/../.." && pwd)/tests/lib.sh"
cd "$qf_lib_root"
CLEAN_PATH="$(qf_clean_path)"
TIMEOUT="$(command -v timeout || true)"
fail=0

run_clean() {
    local exe="$1"; shift
    PATH="$CLEAN_PATH" ${TIMEOUT:+"$TIMEOUT"} 60 "$exe" "$@" 2>&1
}

for art in "${QF_ARTIFACTS[@]}"; do
    name="$(basename "$art")"
    if ! qf_have_artifact "$art"; then qf_report "$name" "跳过（未构建）"; continue; fi
    out="$(run_clean "$art" tests/e2e/data/users.csv "select 1 as x from t limit 1" --raw)"
    rc=$?
    if [ $rc -eq 0 ] && printf '%s' "$out" | grep -q '^x$'; then
        qf_report "$name" "干净 PATH 可运行"
    else
        qf_report "$name" "失败 rc=$rc: $(printf '%s' "$out" | head -1)"
        fail=1
    fi
    deps="$(qf_third_party_deps "$art")"
    kind="$(qf_artifact_kind "$art")"
    if [ -n "$deps" ]; then
        if [ "$kind" = "static" ]; then
            # 静态产物的承诺是"零第三方 DLL"，引用了就是失败，与 dist/ 里有没有同名文件无关。
            qf_report "$name" "静态产物引用了第三方 DLL: $(printf '%s' "$deps" | tr '\n' ' ')"
            fail=1
        fi
        for d in $deps; do
            [ -f "dist/$d" ] || { qf_report "$name" "缺少随包 DLL: $d"; fail=1; }
        done
    else
        qf_report "$name" "第三方 DLL 引用=0"
    fi
done

echo "== 随包 DLL 的依赖闭合 =="
for so in dist/*.dll; do
    [ -f "$so" ] || continue
    for dep in $(objdump -p "$so" 2>/dev/null | awk '/DLL Name/{print $3}'); do
        case "$dep" in
            KERNEL32.dll|USER32.dll|ADVAPI32.dll|WS2_32.dll|ole32.dll|oleaut32.dll|uuid.dll|\
            bcrypt.dll|ntdll.dll|VERSION.dll|USERENV.dll|pathcch.dll|synchronization.dll|\
            userenv.dll|api-ms-win-*|ucrtbase.dll|msvcrt.dll) continue ;;
        esac
        [ -f "dist/$dep" ] || { qf_report "$(basename "$so")" "缺少依赖 $dep"; fail=1; }
    done
done

echo "== Python UDF 零落盘 =="
if ! qf_have_artifact dist/queryforge.exe; then
    qf_report queryforge.exe "跳过（未构建）"
    zd=""
else
zd="$qf_lib_root/out/zd.$$"
mkdir -p "$zd"
zdw="$(winpath "$zd" 2>/dev/null || printf '%s' "$zd")"
if out="$(LOCALAPPDATA="$zdw" TEMP="$zdw" TMP="$zdw" \
        PATH="$CLEAN_PATH" ${TIMEOUT:+"$TIMEOUT"} 60 dist/queryforge.exe \
        tests/e2e/data/users.csv "select score(age) as s from t limit 2" \
        --plugin stages/python/fixtures/score.py --raw 2>&1)"; then
    n="$(find "$zd" -type f 2>/dev/null | wc -l)"
    if [ "$n" -eq 0 ]; then qf_report "queryforge.exe" "UDF 运行后临时目录为空"; else
        qf_report "queryforge.exe" "写入 $n 个文件"; fail=1
    fi
else
    qf_report "queryforge.exe" "UDF 失败"
    fail=1
fi
fi
[ -n "$zd" ] && rm -rf "$zd"

qf_summary "$fail" portability
exit "$fail"
