# Python 零落盘：encodings 必须在 CPython 构建期冻结

目标是"运行期一个字节都不落盘"（与 C 的 hello world 对齐）。实测结论是：`encodings` 必须在**构建 CPython 时**进入 `_PyImport_FrozenModules`，运行期替换 `PyImport_FrozenModules` 指针无效。

## 实测

- 用 `_freeze_module.exe` 把 `Lib/encodings` 的 122 个模块编成 `_frozen` 表（`stages/python/qf_py_frozen.c`），在 `Py_InitializeFromConfig` 之前把默认表复制并追加、赋给 `PyImport_FrozenModules`。
- 结果：初始化仍失败，报
  `Python 初始化失败：failed to get the Python codec of the filesystem encoding`。
- 原因：静态构建里 `Python/import.c` 的 FrozenImporter 读的是编译期数组 `_PyImport_FrozenModules`，运行期赋值只改了一个导出指针，导入机制看不到。`nm` 里 `_PyImport_FrozenModules` 不是全局符号，无法就地追加。

## 正确路线

1. **构建期冻结**：把 `encodings` 加入 CPython 的冻结清单（`Tools/build/freeze_modules.py` 的模块表，或直接改 `Python/frozen_modules/` 的生成清单），重编 `python311.lib`。这一步完成后，解释器不再需要任何磁盘路径即可启动。
2. **运行期内存 finder**：再实现一个从内嵌 zip 字节读取源码的 `meta_path` finder（用内建的 `_struct`/`zlib` 解析 zip，`compile()` 后 `exec`），负责 `json`/`re` 等其余纯 Python 模块。
3. 删除 `payload_materialize`（当前把 `lib.zip` 释放到 `%LOCALAPPDATA%` 的逻辑）。

## 现状与代价

- 当前仍走"内嵌 zip + 首次释放"：单文件、无伴随文件，但首次 UDF 会写一个缓存 zip。
- 已落地的前置件：`scripts/freeze_encodings.py`（生成 122 个模块的冻结表）与 `qf_py_frozen.c`，已在链接里但不启用（`QF_USE_FROZEN_ENCODINGS` 未定义），等 CPython 重编后开启。
- 第 1 步需要重跑 PCbuild（约 10 分钟），第 2 步约 300 行；两者都不改动任何语言阶段的契约。
