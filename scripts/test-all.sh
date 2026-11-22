#!/usr/bin/env bash
# 逐个阶段跑自测 + 端到端。用于提交前自检，不替代各语言的独立 test.sh。
set -uo pipefail
cd "$(dirname "$0")/.."
mkdir -p out
# shellcheck source=/dev/null
. scripts/env.sh

fail=0
PY="$(qf_require_python)" || { echo "找不到 python（阶段清单需要）" >&2; exit 1; }
if ! clist="$("$PY" scripts/stages.py --list)"; then
    echo "阶段清单读取失败" >&2
    exit 1
fi
# c 宿主与 typescript codegen 不是链接阶段，清单不列，单独保留。
for s in c typescript $clist; do
    if [ -x "stages/$s/test.sh" ]; then
        printf '%-12s ' "$s"
        if bash "stages/$s/test.sh" > "out/test-$s.log" 2>&1; then
            echo "ok"
        else
            echo "FAIL"
            sed -n '1,15p' "out/test-$s.log"
            fail=1
        fi
    fi
done

printf '%-12s ' abi
if bash tests/e2e/abi.sh > out/test-abi.log 2>&1; then echo ok; else echo FAIL; sed -n '1,15p' out/test-abi.log; fail=1; fi

printf '%-12s ' portability
if bash tests/e2e/portability.sh > out/test-portability.log 2>&1; then echo ok; else echo FAIL; sed -n '1,15p' out/test-portability.log; fail=1; fi

if [ -f dist/queryforge.exe ] || [ -x dist/queryforge.exe ]; then
    printf '%-12s ' e2e
    if bash tests/e2e/run.sh > out/test-e2e.log 2>&1; then
        echo "ok"
    else
        echo "FAIL"
        sed -n '1,20p' out/test-e2e.log
        fail=1
    fi
fi

exit "$fail"
