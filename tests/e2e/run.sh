#!/usr/bin/env bash
# 端到端验收：同一批 SQL 分别交给 queryforge 与 Python 标准库 sqlite3，
# 逐字节比较 --raw 输出。sqlite3 是唯一可得的权威对照实现（零第三方依赖）。
set -euo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/env.sh"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

PY="$(qf_require "$(command -v python 2>/dev/null || true)" "$(command -v python3 2>/dev/null || true)" \
           "D:/Python/Python311/python.exe" /mnt/d/Python/Python311/python.exe /d/Python/Python311/python.exe)" \
     || { echo "找不到 python" >&2; exit 1; }

mkdir -p out/e2e
BIN="${1:-./dist/queryforge.exe}"
LIMIT="${2:-}"
if [ ! -f "$BIN" ]; then
    bash scripts/build.sh
fi

# 第二个参数给出单条查询超时秒数：混合运行时的装配体可能挂起，
# 没有超时会让一次 e2e 卡到 300 秒。
run_bin() {
    if [ -n "$LIMIT" ]; then timeout "$LIMIT" "$BIN" "$@"; else "$BIN" "$@"; fi
}

fail=0
total=0

run_suite() {
    local csv="$1" qfile="$2" prefix="$3"
    local i=0
    while IFS= read -r sql; do
        [ -z "$sql" ] && continue
        i=$((i + 1))
        total=$((total + 1))
        if ! run_bin "$csv" "$sql" --raw \
                > "out/e2e/got.$prefix.$i.tsv" 2> "out/e2e/got.$prefix.$i.err"; then
            echo "query $prefix#$i 执行失败: $sql"
            sed -n '1,5p' "out/e2e/got.$prefix.$i.err"
            fail=1
            continue
        fi
        "$PY" tests/e2e/oracle.py "$csv" "$sql" > "out/e2e/exp.$prefix.$i.tsv"
        if ! diff -u "out/e2e/exp.$prefix.$i.tsv" "out/e2e/got.$prefix.$i.tsv" \
                > "out/e2e/diff.$prefix.$i.txt"; then
            echo "query $prefix#$i 与 sqlite3 不一致: $sql"
            sed -n '1,20p' "out/e2e/diff.$prefix.$i.txt"
            fail=1
        fi
    done < "$qfile"
}

run_suite tests/e2e/data/users.csv  tests/e2e/queries.txt        users
run_suite tests/e2e/data/groups.csv tests/e2e/groups_queries.txt groups

# 表格渲染（JS 阶段）单独验证：默认输出必须含表头与行数汇总
if run_bin tests/e2e/data/users.csv \
        "select name as name, age as age from t" > out/e2e/table.txt 2> out/e2e/table.err; then
    grep -q "rows" out/e2e/table.txt || { echo "表格输出缺少行数汇总"; fail=1; }
    grep -q "name" out/e2e/table.txt || { echo "表格输出缺少表头"; fail=1; }
else
    echo "表格输出失败"
    sed -n '1,5p' out/e2e/table.err
    fail=1
fi

echo "e2e: $total 条查询, fail=$fail"
exit "$fail"
