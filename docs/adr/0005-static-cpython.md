# Python 走自建静态 CPython，标准库以 payload 形式落地

CPython 的 Windows 构建默认产出 DLL；把 `pythoncore` 的 Configuration Type 改成 Static Library 并定义 `Py_NO_ENABLE_SHARED`，即可得到含全部 C API 的静态库，被 MinGW 主干链接进出。标准库是 `.py` 文件而非代码，无法静态链接，因此打 zip 内嵌、首次运行解包到 `%LOCALAPPDATA%\PolyglotForge\payload-<hash>\`。

## 考虑的方案

- **RustPython + freeze-stdlib**：零释放、纯 MinGW 可链，但语义是 Python 子集且不支持 C 扩展，违背"语言惯用法优先"。
- **直接用 python311.lib（DLL 导入库）**：构建最省事，但目标机需要 `python311.dll`，等于放弃单文件。

## 后果

构建机需要 CPython 源码与 MSVC；产出的静态库与 C# 阶段共用同一套 MSVC CRT 兼容 stub。运行时唯一的磁盘写入是标准库解包，属于资源层而非运行时依赖。
