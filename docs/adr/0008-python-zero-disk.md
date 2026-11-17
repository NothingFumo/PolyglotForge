# Python 零落盘：encodings 必须在 CPython 构建期冻结

目标是"运行期一个字节都不落盘"（与 C 的 hello world 对齐）。实测结论：`encodings` 必须在**构建 CPython 时**进入冻结表；运行期把整张默认表换掉会丢掉 bootstrap，解释器起不来（原因与更正见下）。

## 实测（含一处此前的误判，已更正）

- 用 `_freeze_module.exe` 把 `Lib/encodings` 的 122 个模块编成 `_frozen` 表（`stages/python/qf_py_frozen.c`），在 `Py_InitializeFromConfig` 之前把 `PyImport_FrozenModules` 指向"默认表 + 追加项"。
- 结果：初始化失败，报
  `Python 初始化失败：failed to get the Python codec of the filesystem encoding`。
- **更正**：第一版结论说"导入机制读编译期数组、指针无效"是错的。`Python/frozen.c` 末尾是
  `const struct _frozen *PyImport_FrozenModules = NULL;`，即默认表由 `bootstrap_modules` /
  `stdlib_modules` / `test_modules` 三张表通过 `_PyImport_Frozen*` 指针提供，而
  `PyImport_FrozenModules` 初始为 NULL。我们的代码把 `PyImport_FrozenModules`（NULL）当成默认表来复制，
  于是 n=0，**整体替换后丢掉了 bootstrap 表**（importlib 引导模块），这才是启动失败的原因。
- 真正的运行期做法应是：把我们的表与 `_PyImport_FrozenBootstrap`/`_PyImportFrozenStdlib` 一并拼接，
  而不是只读 `PyImport_FrozenModules`。但更稳妥、可复现的做法仍是构建期冻结（见下）。

## 正确路线

1. **构建期冻结**：直接改 `Python/frozen.c` 的 `stdlib_modules[]` 数组（把 `stages/python/frozen/*.h`
   复制进 `Python/frozen_modules/` 并在数组里追加 122 个 `encodings.*` 条目），重编 `python311.lib`。
   目录里已有 `scripts/freeze_encodings.py` 生成的头文件，只差搬运与重编。
2. **运行期内存 finder**：再实现一个从内嵌 zip 字节读取源码的 `meta_path` finder（用内建的 `_struct`/`zlib`
   解析 zip，`compile()` 后 `exec`），负责 `json`/`re` 等其余纯 Python 模块。
3. 删除 `payload_materialize`（当前把 `lib.zip` 释放到 `%LOCALAPPDATA%` 的逻辑）。

## 现状与代价

- 当前仍走"内嵌 zip + 首次释放"：单文件、无伴随文件，但首次 UDF 会写一个缓存 zip。
- 已落地的前置件：`scripts/freeze_encodings.py`（生成 122 个模块的冻结表）与 `qf_py_frozen.c`，已在链接里但不启用（`QF_USE_FROZEN_ENCODINGS` 未定义），等 CPython 重编后开启。
- 第 1 步需要重跑 PCbuild（约 10 分钟），第 2 步约 300 行；两者都不改动任何语言阶段的契约。

### 追加：分类指针同样不可改（最终结论）

第二、三次尝试分别把扩展表接到 `PyImport_FrozenModules`（NULL 覆盖指针）与
`_PyImport_FrozenStdlib`（分类指针）上，初始化仍报同一条 `failed to get the Python
codec`。说明 3.11 的 `bootstrap_modules[] / stdlib_modules[]` 是 **`static const`**，
导入机制直接读数组，任何运行期指针改写都无效。

因此唯一可行路线是**构建期改 `Python/frozen.c` 的 `stdlib_modules[]`**：把
`stages/python/frozen/*.h` 覆制到 `Python/frozen_modules/`，按 5 字段 `_frozen`
（name/code/size/is_package/get_code，get_code 传 NULL）追加 122 条，再重编
`python311.lib`。`stages/python/zip_finder.py`（内存 zip 导入器）与
`scripts/embed_text.py`（把脚本转成 C 字符串）已就位，等重编后接线。
