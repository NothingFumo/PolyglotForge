"""权威对照实现：用 Python 标准库 sqlite3 执行同一条 SQL，输出 TSV。

用法：python oracle.py <csv> <sql>
约定（与 queryforge --raw 对齐）：
  - 表名固定为 t；
  - 空值输出为空串；
  - 浮点用 %g 格式化；
  - 列名按 SQL 的 select 项文本输出。
"""
import csv
import io
import sqlite3
import sys

# 输出统一用 LF：宿主 exe 也是 LF，逐字节比较才有意义。
sys.stdout.reconfigure(newline="\n")


def infer(rows):
    """按列推断类型：全整数→INTEGER，全数值→REAL，否则 TEXT；空串→NULL。"""
    cols = len(rows[0])
    types = []
    for c in range(cols):
        vals = [r[c] for r in rows if r[c] != ""]
        if not vals:
            types.append("TEXT")
            continue
        if all(v.lstrip("-").isdigit() for v in vals):
            types.append("INTEGER")
        else:
            try:
                for v in vals:
                    float(v)
                types.append("REAL")
            except ValueError:
                types.append("TEXT")
    return types


def fmt(v):
    if v is None:
        return ""
    if isinstance(v, float):
        return "%g" % v
    return str(v)


def main():
    path, sql = sys.argv[1], sys.argv[2]
    with io.open(path, "r", encoding="utf-8", newline="") as f:
        rows = list(csv.reader(f))
    header, body = rows[0], rows[1:]
    types = infer(body)

    con = sqlite3.connect(":memory:")
    decl = ", ".join('"%s" %s' % (name, t) for name, t in zip(header, types))
    con.execute("CREATE TABLE t (%s)" % decl)
    placeholders = ", ".join("?" * len(header))
    con.executemany(
        "INSERT INTO t VALUES (%s)" % placeholders,
        [[None if v == "" else v for v in r] for r in body],
    )

    cur = con.execute(sql)
    names = [d[0] for d in cur.description]
    print("\t".join(names))
    for row in cur.fetchall():
        print("\t".join(fmt(v) for v in row))


if __name__ == "__main__":
    main()
