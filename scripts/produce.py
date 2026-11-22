#!/usr/bin/env python3
"""装配器调用产物生成器的唯一入口。

cmd 侧的引号与路径规则很容易把 `"bash" scripts\\produce.sh x` 解释坏（实测会
把 .sh 当批处理逐行执行）。这里用 python 作为 shim：自己找到可用的 bash，
把参数原样转交，并透传退出码。
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from env import find_bash  # noqa: E402  （env.py 顶层只做定义，可安全导入）


def main(argv):
    if len(argv) != 1:
        print("用法: produce.py <target>", file=sys.stderr)
        return 2
    bash = find_bash()
    if not bash:
        print("找不到可用的 bash（需要 Git for Windows 或 MSYS）", file=sys.stderr)
        return 1
    cmd = [bash, os.path.join(ROOT, "scripts", "produce.sh"), argv[0]]
    return subprocess.run(cmd, cwd=ROOT).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
