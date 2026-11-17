#!/usr/bin/env bash
# 逐个阶段跑自测 + 端到端。用于提交前自检，不替代各语言的独立 test.sh。
set -uo pipefail
cd "$(dirname "$0")/.."
mkdir -p out
# shellcheck source=/dev/null
. scripts/env.sh

fail=0
for s in c typescript cpp rust go csharp kotlin python js; do
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
