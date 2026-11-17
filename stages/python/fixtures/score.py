"""查询期 UDF 插件示例。

契约类型只有 整数 / 浮点 / 文本 / 空值，因此 row 本身就是单个值：
宿主把每个 qf_value 直接转成 Python 的 int / float / str / None。
插件里所有顶层函数都可以被查询按名字调用。
"""


def score(row):
    """整数入参：score(30) == 60。"""
    return row * 2


def total(amount, rate):
    """两参 + 浮点提升：total(30, 1.5) == 45.0。"""
    return amount * rate


def label(row):
    """文本返回值：label(60) == "value=60"。"""
    return "value=%d" % row


def nothing(row):
    """返回 None，宿主侧映射成空值。"""
    return None


def stdlib(row):
    """证明载荷里的标准库可用：UDF 常常需要 import。"""
    import json

    return json.dumps({"n": row})


def boom(row):
    """故意抛异常，用于验证 QF_ERR_UDF 与诊断文本。"""
    raise ValueError("插件内部错误")
