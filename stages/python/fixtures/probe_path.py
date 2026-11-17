"""探针插件：返回标准库模块的来源路径。

用来证明标准库来自 exe 内嵌的 zip 载荷（路径里含 lib.zip），
而不是本机安装的 CPython（路径会落在 Python311\\Lib）。
"""


def stdlib_origin(row):  # noqa: ARG001 - UDF 参数由宿主传入
    import json

    path = getattr(json, "__file__", "") or "<builtin>"
    return path
