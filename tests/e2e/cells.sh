#!/usr/bin/env bash
# 四产物的运行时可移植性检查：清空 PATH（只留系统目录），逐个启动产物。
# 动态格必须自带第三方 DLL；静态格不得依赖任何非系统 DLL。
set -uo pipefail
cd "$(dirname "$0")/../.."
CLEAN_PATH="/c/Windows/system32:/c/Windows"
TIMEOUT="$(command -v timeout || true)"
fail=0

run_cell() {
    local exe="$1" need="$2"
    [ -f "$exe" ] || { printf '%-34s 跳过（未构建）\n' "$(basename "$exe")"; return 0; }
    local out
    out=$(PATH="$CLEAN_PATH" ${TIMEOUT:+"$TIMEOUT"} 40 "$exe" tests/e2e/data/users.csv "select 1 as x from t limit 1" --raw 2>&1)
    local rc=$?
    if [ $rc -eq 0 ] && printf '%s' "$out" | grep -q '^x$'; then
        printf '%-34s 干净 PATH 可运行（%s）\n' "$(basename "$exe")" "$need"
    else
        printf '%-34s 失败 rc=%s：%s\n' "$(basename "$exe")" "$rc" "$(printf '%s' "$out" | head -1)"
        fail=1
    fi
}

echo "== 动态格：第三方 DLL 必须随包 =="
for d in libstdc++-6.dll libwinpthread-1.dll vcruntime140.dll vcruntime140_1.dll; do
    if [ -f "dist/$d" ]; then printf '  dist/%-26s 存在\n' "$d"; else printf '  dist/%-26s 缺失\n' "$d"; fi
done
run_cell dist/queryforge-deps.exe "libstdc++-6/winpthread 随包"
run_cell dist/queryforge-all.exe  "vcruntime140 随包"

echo "== 静态格：不得依赖非系统 DLL =="
for exe in dist/queryforge.exe dist/queryforge-static-msvc.exe; do
    [ -f "$exe" ] || continue
    bad=$(objdump -p "$exe" 2>/dev/null | grep "DLL Name" | awk '{print $3}' \
          | grep -icE "vcruntime|libstdc|winpthread")
    printf '  %-34s 非系统 DLL 引用=%s\n' "$(basename "$exe")" "$bad"
    [ "$bad" -eq 0 ] || fail=1
done
run_cell dist/queryforge.exe "零第三方 DLL"
run_cell dist/queryforge-static-msvc.exe "零第三方 DLL"

[ $fail -eq 0 ] && echo "cells: 4/4 通过" || echo "cells: 有失败项"
exit $fail
