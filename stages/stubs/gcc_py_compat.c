/*
 * gcc 链接 MSVC 编译的 CPython 时的补充符号。
 * __isa_available 是 MSVC CRT 的 CPU 分派变量；本项目不依赖其取值，
 * 置 0 走保守路径。放在独立文件里，避免与 C#/CRT 兼容层互相干扰。
 */
int __isa_available = 0;
