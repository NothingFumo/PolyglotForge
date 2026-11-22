"""错误路径插件：验证 UDF 异常会中止查询，而不是静默产出 NULL。"""


def boom(value):
    raise ValueError("udf exploded")
