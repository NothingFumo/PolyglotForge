#!/usr/bin/env bash
# 验收共享库：三个入口（run/portability/abi/test-all）共用同一套工具发现、
# 产物清单与失败报告。产物清单来自 scripts/stages.py，不再各自硬编码。
set -uo pipefail

qf_lib_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=/dev/null
. "$qf_lib_root/scripts/env.sh"

QF_PY="$(qf_require_python)" || { echo "找不到 python（验收需要）" >&2; return 1 2>/dev/null || exit 1; }
QF_ARTIFACTS=()
while IFS= read -r line; do
    [ -n "$line" ] && QF_ARTIFACTS+=("$line")
done < <("$QF_PY" "$qf_lib_root/scripts/stages.py" --artifacts)

# 干净 PATH：只保留系统目录，用于检验"随包 DLL 是否齐全"。
qf_clean_path() { printf '%s' "/c/Windows/system32:/c/Windows"; }

# 产物依赖的非系统 DLL（vcruntime/libstdc++/winpthread 等）。
qf_third_party_deps() {
    local exe="$1"
    objdump -p "$exe" 2>/dev/null | awk '/DLL Name/{print $3}' \
        | grep -iE "vcruntime|libstdc|libgcc|winpthread" | sort -u
}

qf_have_artifact() { [ -f "$1" ]; }

# 产物声明的种类（static / dynamic），来自清单而不是猜测。
qf_artifact_kind() {
    "$QF_PY" "$qf_lib_root/scripts/stages.py" --kind "$1" 2>/dev/null
}

qf_report() { printf '  %-34s %s\n' "$1" "$2"; }

qf_summary() {
    local fail="$1" label="$2"
    if [ "$fail" -eq 0 ]; then echo "$label: 通过"; else echo "$label: 有失败项"; fi
    return "$fail"
}
