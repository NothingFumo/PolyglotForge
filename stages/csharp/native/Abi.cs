// 跨语言布局：与 contracts/qf.h + gen/qf_layout.h 逐字段对齐。
// 这一层只做数据搬运，不含任何谓词语义。
using System;
using System.Runtime.InteropServices;

namespace QfCs;

internal static class QfErr
{
    public const int Ok = 0;
    public const int Arg = 1;
    public const int Io = 2;
    public const int Parse = 3;
    public const int Type = 4;
    public const int Plan = 5;
    public const int Exec = 6;
    public const int Udf = 7;
    public const int Panic = 8;
    public const int Unsupported = 9;
}

internal static class QfT
{
    public const int Null = 0;
    public const int Int = 1;
    public const int Double = 2;
    public const int Text = 3;
}

internal static class QfNodeKind
{
    public const int Lit = 1;
    public const int Col = 2;
    public const int Bin = 3;
    public const int Unary = 4;
    public const int Call = 5;
    public const int Agg = 6;
    public const int Star = 7;
}

internal static class QfOp
{
    public const int None = 0;
    public const int Eq = 1;
    public const int Ne = 2;
    public const int Lt = 3;
    public const int Le = 4;
    public const int Gt = 5;
    public const int Ge = 6;
    public const int And = 7;
    public const int Or = 8;
    public const int Not = 9;
    public const int Add = 10;
    public const int Sub = 11;
    public const int Mul = 12;
    public const int Div = 13;
    public const int Mod = 14;
    public const int Neg = 15;
}

internal static class QfFlags
{
    public const int TypeShift = 0;
    public const int TypeMask = 15;
    public const int Desc = 16;
    public const int Resolved = 32;
}

/// <summary>qf_str：文本一律 (ptr, len)，不假设 NUL 结尾。</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct QfStr
{
    public byte* Ptr;
    public uint Len;
}

/// <summary>qf_value：type 在 0，联合体在 8（最大成员 qf_str 为 16 字节），共 24 字节。</summary>
[StructLayout(LayoutKind.Explicit, Size = 24)]
internal struct QfValue
{
    [FieldOffset(0)] public int Type;
    [FieldOffset(8)] public long I;
    [FieldOffset(8)] public double D;
    [FieldOffset(8)] public QfStr S;
}

/// <summary>qf_node：kind/op/flags/a/b/argc 之后是 lit（对齐 8）与 name，共 56 字节。</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct QfNode
{
    public byte Kind;
    public byte Op;
    public ushort Flags;
    public int A;
    public int B;
    public int Argc;
    public QfValue Lit;
    public QfStr Name;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct QfAst
{
    public QfNode* Nodes;
    public uint NodeCount;
    public int* Select;
    public uint SelectCount;
    public QfStr FromTable;
    public int WhereRoot;
    public int* GroupBy;
    public uint GroupCount;
    public int* OrderBy;
    public uint OrderCount;
    public long Limit;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct QfCol
{
    public QfStr Name;
    public int Type;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct QfSchema
{
    public uint Count;
    public QfCol* Cols;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct QfDiag
{
    public int Code;
    public uint Line;
    public uint Col;
    public QfStr Message;
    public QfStr Hint;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct QfHost
{
    public delegate* unmanaged<byte*, ulong, ulong, byte*> Alloc;
    public delegate* unmanaged<byte*, byte*, uint, QfStr> Str;
    public byte* Ctx;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct QfBatch
{
    public uint Rows;
    public uint Cols;
    public QfValue* Values;
}

internal static unsafe class Abi
{
    /// <summary>把 (ptr, len) 的 UTF-8 文本复制成托管字符串。</summary>
    public static string Text(byte* ptr, uint len)
    {
        if (ptr == null || len == 0) return string.Empty;
        return System.Text.Encoding.UTF8.GetString(ptr, checked((int)len));
    }
}
