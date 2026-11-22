#!/usr/bin/env bash
# 契约测试：每个阶段归档导出的 qf_* 符号，必须都能在 contracts/qf.h 里找到声明。
# 这条测试专门盯"契约外入口"——qf_js_where_named 就是这样被发现的。
set -uo pipefail
. "$(cd "$(dirname "$0")/../.." && pwd)/tests/lib.sh"

cd "$qf_lib_root"
declared="$(grep -oE '\bqf_[a-z0-9_]+[[:space:]]*\(' contracts/qf.h | tr -d ' (' | sort -u)"
fail=0
# Kotlin/Native 不能按值传 struct，qf_dsl_build 由 stages/kotlin/shim.c 包一层；
# 被包的那个实现符号是工具链必需的内部 seam，允许它不出现在 qf.h。
ALLOWED="qf_dsl_build_impl"

check_archive() {
    local name="$1" archive="$2"
    [ -f "$archive" ] || { qf_report "$name" "跳过（归档不存在）"; return 0; }
    local exported leaks
    exported="$(nm --defined-only -g "$archive" 2>/dev/null | awk '{print $NF}' | grep -E '^qf_[a-z0-9_]+$' | sort -u)"
    leaks="$(comm -23 <(printf '%s\n' "$exported") <(printf '%s\n' "$declared") \
             | grep -vxF "$ALLOWED" || true)"
    if [ -n "$leaks" ]; then
        qf_report "$name" "契约外导出：$(printf '%s' "$leaks" | tr '\n' ' ')"
        fail=1
    else
        qf_report "$name" "导出 $(printf '%s\n' "$exported" | grep -c . ) 个符号，全部在 qf.h"
    fi
}

# 反向检查：契约里属于该阶段的入口必须真的导出，缺了只能等到链接期才炸。
check_exports() {
    local name="$1" archive="$2"; shift 2
    [ -f "$archive" ] || { qf_report "$name" "跳过（归档不存在）"; return 0; }
    local sym missing=""
    # rust 的导出是 weak/局部符号，-g 过滤会漏掉，故逐符号直接查归档。
    for sym in "$@"; do
        nm --defined-only "$archive" 2>/dev/null | awk '{print $NF}' | grep -x "$sym" >/dev/null \
            || missing="$missing $sym"
    done
    if [ -n "$missing" ]; then
        qf_report "$name" "缺少契约入口:$missing"
        fail=1
    else
        qf_report "$name" "契约入口齐全（$# 个）"
    fi
}

check_exports cpp    stages/cpp/libqf_cpp.a    qf_sql_parse
check_exports rust   stages/rust/libqf_rust.a  qf_plan_build
check_exports go     stages/go/out/libqf_go.a  qf_csv_scan
check_exports kotlin stages/kotlin/build/libqf_kt.a qf_dsl_build qf_kt_batch
check_exports js     stages/js/out/libqf_js.a  qf_js_init qf_js_format qf_js_where qf_js_where_named

check_archive cpp    stages/cpp/libqf_cpp.a
check_archive rust   stages/rust/libqf_rust.a
check_archive go     stages/go/out/libqf_go.a
check_archive kotlin stages/kotlin/build/libqf_kt.a
check_archive js     stages/js/out/libqf_js.a

qf_summary "$fail" abi
exit "$fail"
