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
run_suite tests/e2e/data/wide.csv   tests/e2e/wide_queries.txt   wide
run_suite tests/e2e/data/quoted.csv tests/e2e/quoted_queries.txt quoted
run_suite tests/e2e/data/empty.csv  tests/e2e/empty_queries.txt  empty

check_same() { # label fileA fileB
    local label="$1" a="$2" b="$3"
    total=$((total + 1))
    if ! diff -u "$a" "$b" > "out/e2e/diff.$label.txt"; then
        echo "$label 输出不一致"
        sed -n '1,20p' "out/e2e/diff.$label.txt"
        fail=1
    fi
}

# --where（JS 沙箱）与等价 SQL 必须给出相同行
if run_bin tests/e2e/data/users.csv "select name as name from t" \
        --where "row.age > 30 && row.city !== 'SZ'" --raw \
        > out/e2e/where_js.tsv 2> out/e2e/where_js.err; then
    if run_bin tests/e2e/data/users.csv \
            "select name as name from t where age > 30 and city <> 'SZ'" --raw \
            > out/e2e/where_sql.tsv 2> out/e2e/where_sql.err; then
        check_same where out/e2e/where_sql.tsv out/e2e/where_js.tsv
    else
        echo "--where 对照 SQL 执行失败"
        sed -n '1,5p' out/e2e/where_sql.err
        fail=1
    fi
else
    echo "--where 执行失败"
    sed -n '1,5p' out/e2e/where_js.err
    fail=1
fi

# Kotlin DSL 与等价 SQL
if run_bin tests/e2e/data/users.csv --dsl 'from("t"){select("name");limit(2)}' --raw \
        > out/e2e/dsl.tsv 2> out/e2e/dsl.err; then
    if run_bin tests/e2e/data/users.csv "select name as name from t limit 2" --raw \
            > out/e2e/dsl_sql.tsv 2> out/e2e/dsl_sql.err; then
        check_same dsl out/e2e/dsl_sql.tsv out/e2e/dsl.tsv
    else
        echo "DSL 对照 SQL 执行失败"
        sed -n '1,5p' out/e2e/dsl_sql.err
        fail=1
    fi
else
    echo "Kotlin DSL 执行失败"
    sed -n '1,5p' out/e2e/dsl.err
    fail=1
fi

# Python UDF 与等价算术
if run_bin tests/e2e/data/users.csv "select score(age) as s from t where age > 0" \
        --plugin stages/python/fixtures/score.py --raw \
        > out/e2e/udf.tsv 2> out/e2e/udf.err; then
    if run_bin tests/e2e/data/users.csv "select age * 2 as s from t where age > 0" --raw \
            > out/e2e/udf_sql.tsv 2> out/e2e/udf_sql.err; then
        check_same udf out/e2e/udf_sql.tsv out/e2e/udf.tsv
    else
        echo "UDF 对照 SQL 执行失败"
        sed -n '1,5p' out/e2e/udf_sql.err
        fail=1
    fi
else
    echo "Python UDF 执行失败"
    sed -n '1,5p' out/e2e/udf.err
    fail=1
fi

# 分片数不影响结果
if run_bin tests/e2e/data/users.csv "select name as name from t order by name asc" \
        --shards 1 --raw > out/e2e/shard1.tsv 2> out/e2e/shard1.err; then
    if run_bin tests/e2e/data/users.csv "select name as name from t order by name asc" \
            --shards 8 --raw > out/e2e/shard8.tsv 2> out/e2e/shard8.err; then
        check_same shards out/e2e/shard1.tsv out/e2e/shard8.tsv
    else
        echo "--shards 8 执行失败"
        sed -n '1,5p' out/e2e/shard8.err
        fail=1
    fi
else
    echo "--shards 1 执行失败"
    sed -n '1,5p' out/e2e/shard1.err
    fail=1
fi

# Kotlin 协程批处理：同一输入重复执行必须稳定
if run_bin tests/e2e/data/users.csv "select count(*) as n from t" \
        --batch "select count(*) from t;select name from t limit 2" \
        > out/e2e/batch1.tsv 2> out/e2e/batch1.err; then
    if run_bin tests/e2e/data/users.csv "select count(*) as n from t" \
            --batch "select count(*) from t;select name from t limit 2" \
            > out/e2e/batch2.tsv 2> out/e2e/batch2.err; then
        check_same batch out/e2e/batch1.tsv out/e2e/batch2.tsv
        grep -q '^query 1: 1 rows$' out/e2e/batch1.tsv || { echo "batch 第 1 条行数不符"; fail=1; }
        grep -q '^query 2: 2 rows$' out/e2e/batch1.tsv || { echo "batch 第 2 条行数不符"; fail=1; }
    else
        echo "batch 第二次执行失败"
        sed -n '1,5p' out/e2e/batch2.err
        fail=1
    fi
else
    echo "batch 执行失败"
    sed -n '1,5p' out/e2e/batch1.err
    fail=1
fi

# 错误路径：非零退出必须带可读诊断
expect_error() { # label want_rc stderr_regex -- command...
    local label="$1" want_rc="$2" want_err="$3"
    shift 3
    local err rc=0
    err="$("$@" 2>&1 >/dev/null)" || rc=$?
    total=$((total + 1))
    if [ "$rc" -ne "$want_rc" ]; then
        echo "$label: rc=$rc，期望 $want_rc"
        fail=1
    fi
    if ! printf '%s' "$err" | grep -q "$want_err"; then
        echo "$label: stderr 缺少 '$want_err'（实际：$(printf '%s' "$err" | head -1)）"
        fail=1
    fi
}

expect_error err_column 65 'error\[type\]: 表 t 中不存在列' \
    run_bin tests/e2e/data/users.csv "select nope from t" --raw
expect_error err_parse 65 'error\[parse\]' \
    run_bin tests/e2e/data/users.csv "select from t" --raw
expect_error err_type 65 '比较两侧类型不兼容' \
    run_bin tests/e2e/data/users.csv "select name from t where name > 1" --raw
expect_error err_order 65 'ORDER BY 无法对应到输出列' \
    run_bin tests/e2e/data/users.csv "select name from t order by age" --raw
expect_error err_opt 64 '未知选项' \
    run_bin tests/e2e/data/users.csv "select name from t" --nope
expect_error err_udf 65 'udf exploded' \
    run_bin tests/e2e/data/users.csv "select boom(age) as b from t" \
    --plugin stages/python/fixtures/boom.py --raw
expect_error err_udf_args 65 'UDF 参数过多' \
    run_bin tests/e2e/data/users.csv \
    "select boom(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17) as b from t" \
    --plugin stages/python/fixtures/boom.py --raw

# --explain 必须反映实际算子链，而不是固定模板
explain_fail=0
if run_bin tests/e2e/data/users.csv "select name as name from t where age > 30" \
        --explain > out/e2e/explain_where.txt 2> out/e2e/explain_where.err; then
    grep -qx 'Project (1 cols)' out/e2e/explain_where.txt || explain_fail=1
    grep -qx '  Filter' out/e2e/explain_where.txt || explain_fail=1
    grep -qx '    Scan t  (4 cols)' out/e2e/explain_where.txt || explain_fail=1
    if grep -q '^Limit' out/e2e/explain_where.txt; then explain_fail=1; fi
    if grep -q '^Sort' out/e2e/explain_where.txt; then explain_fail=1; fi
else
    echo "--explain（WHERE）执行失败"
    sed -n '1,5p' out/e2e/explain_where.err
    explain_fail=1
fi

if run_bin tests/e2e/data/users.csv "select name as name from t order by name asc limit 1" \
        --explain > out/e2e/explain_sort.txt 2> out/e2e/explain_sort.err; then
    grep -qx 'Limit 1' out/e2e/explain_sort.txt || explain_fail=1
    grep -qx '  Sort (1 keys)' out/e2e/explain_sort.txt || explain_fail=1
    grep -qx '    Project (1 cols)' out/e2e/explain_sort.txt || explain_fail=1
    grep -qx '      Scan t  (4 cols)' out/e2e/explain_sort.txt || explain_fail=1
else
    echo "--explain（ORDER BY/LIMIT）执行失败"
    sed -n '1,5p' out/e2e/explain_sort.err
    explain_fail=1
fi

if run_bin tests/e2e/data/users.csv "select city as city, count(*) as n from t group by city" \
        --explain > out/e2e/explain_group.txt 2> out/e2e/explain_group.err; then
    grep -qx 'Project (2 cols)' out/e2e/explain_group.txt || explain_fail=1
    grep -qx '  Aggregate (1 groups)' out/e2e/explain_group.txt || explain_fail=1
    grep -qx '    Scan t  (4 cols)' out/e2e/explain_group.txt || explain_fail=1
    if grep -q 'Filter' out/e2e/explain_group.txt; then explain_fail=1; fi
else
    echo "--explain（GROUP BY）执行失败"
    sed -n '1,5p' out/e2e/explain_group.err
    explain_fail=1
fi

if [ "$explain_fail" -ne 0 ]; then
    echo "--explain 输出与算子链不一致"
    fail=1
fi
total=$((total + 3))

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
