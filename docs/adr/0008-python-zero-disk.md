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

1. **构建期冻结**：`scripts/freeze_encodings.py --install` 生成 122 个头文件，复制进
   `Python/frozen_modules/`，并写入 `Python/frozen.c` 的 include 块与 `stdlib_modules[]`
   条目（`encodings` 的 `is_package` 为 1）。`build_static_python.sh` 在构建 `pythoncore`
   之前调用它，因此"构建期冻结"是可复现的，不依赖任何手工改动。
2. **运行期内存 finder**：再实现一个从内嵌 zip 字节读取源码的 `meta_path` finder（用内建的 `_struct`/`zlib`
   解析 zip，`compile()` 后 `exec`），负责 `json`/`re` 等其余纯 Python 模块。
3. 删除 `payload_materialize`（把 `lib.zip` 释放到 `%LOCALAPPDATA%` 的旧逻辑）。**已完成**。

## 现状

- 运行期不落盘：标准库由内嵌 zip 的内存 finder 提供，encodings 来自构建期冻结表。
- `qf_py_frozen.c`（运行期拼表）与 `QF_USE_FROZEN_ENCODINGS` 已删除；链接里只有
  `qf_py.obj`，冻结内容在 `python311.lib` 里。
- 载荷以只读段链入：`ld -r -b binary` 生成对象，再由 `objcopy --rename-section
  .data=.rdata,...` 改名（ld 的 PE 目标不支持该选项），
  `objdump -h` 里能看到 zip 落在 `.rdata`。
- 代价：重编 `python311.lib` 需要 PCbuild（增量约 30 秒，全量约 10 分钟）。

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
