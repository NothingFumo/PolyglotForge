"""探针插件：报告 encodings 与 json 的来源。

- encodings 应是 `frozen`：它必须在导入机制可用前就存在（构建期冻结）。
- json 的 origin 必须指向内嵌 zip 的成员路径；`<builtin>` 或空值都不能证明
  标准库来自载荷，所以这里统一读 `__spec__.origin`。
"""


def _origin(mod):
    spec = getattr(mod, "__spec__", None)
    if spec is not None and spec.origin:
        return spec.origin
    return "?"


def stdlib_origin(row):  # noqa: ARG001 - UDF 参数由宿主传入
    import encodings
    import json

    return "%s | %s" % (_origin(encodings), _origin(json))
