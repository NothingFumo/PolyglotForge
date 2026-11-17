"""把文本文件转成 C 字符串常量头文件，供内嵌脚本使用。

用法：python scripts/embed_text.py <输入> <输出.h> <符号名>
"""
import sys


def main():
    if len(sys.argv) != 4:
        print("用法: embed_text.py <输入> <输出.h> <符号名>", file=sys.stderr)
        return 2
    src, out, name = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(src, "rb") as f:
        data = f.read()
    lines = ['/* 由 scripts/embed_text.py 生成，勿手改。 */',
             "static const char %s[] = {" % name]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + ", ".join("0x%02x" % b for b in chunk) + ",")
    lines.append("    0x00")
    lines.append("};")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("embed: %s -> %s (%d bytes)" % (src, out, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
