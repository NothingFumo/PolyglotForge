"""探针插件：报告 encodings 与 json 的来源。

- encodings 应显示 `frozen`：它必须在导入机制可用前就存在（零落盘启动的前提）。
- json 应落在内嵌 zip 载荷里（后续由内存 finder 接管时应显示内存来源）。
"""


def stdlib_origin(row):  # noqa: ARG001 - UDF 参数由宿主传入
    import encodings
    import json

    enc = encodings.__spec__.origin if encodings.__spec__ else "?"
    js = getattr(json, "__file__", "") or "<builtin>"
    return "%s | %s" % (enc, js)
