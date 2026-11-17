# C# 与 CPython 的静态库暂不参与默认链接

两者的产物都能独立构建并通过各自自测，但把 MSVC 编译的静态库交给 MinGW 主干链接时，缺的不再是几个兼容符号，而是整套 CRT/C++ 运行时边界。默认构建因此只链接 MinGW 可消费的阶段（C / C++ / Rust / Go / Kotlin / JS），C# 与 Python 保留为「已验证可独立运行」的阶段，`--link-msvc` 作为实验开关保留。

## 实测证据

- **C#（NativeAOT 静态库）**：`qf_cs.lib` 18 MB，`dumpbin` 可见三个导出符号；用 MSVC `link.exe` + `bootstrapperdll.obj` 链接后自测通过（`WHERE age > 30` → keep 1/0/1）。但 gcc 链接需要约 3000 处 `Rhp*` 运行时助手、MSVC 的 C++ operator new/delete（`??_U@...`）与 `std::nothrow` 常量；补齐 `Runtime.WorkstationGC.lib` 与 `aotminipal.lib` 后仍有 C++ 运算符与版本化 WinAPI 导入（`WaitOnAddress`）。C++ 名字修饰与 ABI 无法由 MinGW 提供。
- **CPython 静态库**：`python311.lib` 33 MB（真静态，含 `Py_Initialize`），MSVC 链接自测通过（UDF `score(30)==60`）。gcc 链接剩余缺口集中在 UCRT 导入（`fprintf/sprintf/snprintf`）、`__chkstk`、`__isa_available`、`_Py_atomic_load_64bit_impl` 与 `PathCchSkipRoot`。

## 后续路线（任一即可恢复 9/9）

1. **第二装配器**：用 MSVC `link.exe` 做最终链接，改为验证 Go `c-archive` 与 Kotlin/Native 归档能否被 MSVC 消费。
2. **换实现**：Python 改用 RustPython（`freeze-stdlib` + `capi`），彻底绕开 MSVC CRT；C# 则必须走第二装配器，没有等价替代。

在路线确定前，不把这两个阶段塞进默认链接——让构建保持绿色，比让链接"看起来全绿"更重要。
