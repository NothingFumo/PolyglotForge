#!/usr/bin/env bash
# JavaScript 阶段构建：编 QuickJS → libquickjs.a，再与 qf_js.c 合成 libqf_js.a。
# 产物：stages/js/out/libquickjs.a、stages/js/out/libqf_js.a
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

pick() {
    for c in "$@"; do
        if [ -n "$c" ] && [ -x "$c" ]; then printf '%s' "$c"; return 0; fi
    done
    return 1
}

CC="$(pick "$(command -v gcc 2>/dev/null || true)" /mnt/c/Strawberry/c/bin/gcc.exe /c/Strawberry/c/bin/gcc.exe)" \
    || { echo "找不到 gcc" >&2; exit 1; }
AR="$(pick /mnt/c/Strawberry/c/bin/ar.exe /c/Strawberry/c/bin/ar.exe "$(command -v ar 2>/dev/null || true)")" \
    || { echo "找不到 ar" >&2; exit 1; }

cd "$root"

QJ="stages/js/vendor/quickjs"
OUT="stages/js/out"
OBJ="$OUT/quickjs"

[ -f "$QJ/quickjs.c" ] || { echo "缺少 vendored QuickJS: $QJ" >&2; exit 1; }

mkdir -p "$OBJ"
rm -f "$OUT/libquickjs.a" "$OUT/libqf_js.a" "$OBJ"/*.o

# QuickJS 本体。quickjs.c 依赖 regexp/unicode/cutils/dtoa。
QJS_VER="$(cat "$QJ/VERSION")"
for f in quickjs cutils dtoa libregexp libunicode; do
    "$CC" -std=gnu11 -O2 -fno-strict-aliasing -D_GNU_SOURCE \
          -DCONFIG_VERSION="\"$QJS_VER\"" \
          -I"$QJ" -c "$QJ/$f.c" -o "$OBJ/$f.o"
done

"$AR" rcs "$OUT/libquickjs.a" "$OBJ/quickjs.o" "$OBJ/cutils.o" "$OBJ/dtoa.o" \
      "$OBJ/libregexp.o" "$OBJ/libunicode.o"

# 阶段实现。
"$CC" -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
      -Igen -Icontracts -Istages/js -I"$QJ" \
      -c stages/js/qf_js.c -o "$OUT/qf_js.o"

# 自包含：链接方只需 libqf_js.a 即可拿到 QuickJS 符号。
"$AR" rcs "$OUT/libqf_js.a" "$OUT/qf_js.o" "$OBJ/quickjs.o" "$OBJ/cutils.o" \
      "$OBJ/dtoa.o" "$OBJ/libregexp.o" "$OBJ/libunicode.o"

echo "[js] $OUT/libquickjs.a $OUT/libqf_js.a"
