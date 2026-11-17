/* 由 schema/qf.ts + codegen/gen.ts 生成，勿手改 */

using System.Runtime.InteropServices;

namespace Qf;

public static class Abi
{
    public const int Version = 1;
}

public enum QfErr : int
{
    Ok = 0,
    Arg = 1,
    Io = 2,
    Parse = 3,
    Type = 4,
    Plan = 5,
    Exec = 6,
    Udf = 7,
    Panic = 8,
    Unsupported = 9,
}

public enum QfType : int
{
    Null = 0,
    Int = 1,
    Double = 2,
    Text = 3,
}

[StructLayout(LayoutKind.Explicit, Size = 16)]
public unsafe struct QfStr
{
    [FieldOffset(0)] public byte* Ptr;
    [FieldOffset(8)] public uint Len;
}

[StructLayout(LayoutKind.Explicit, Size = 16)]
public unsafe struct QfValueUnion
{
    [FieldOffset(0)] public long I;
    [FieldOffset(0)] public double D;
    [FieldOffset(0)] public QfStr S;
}

[StructLayout(LayoutKind.Explicit, Size = 24)]
public unsafe struct QfValue
{
    [FieldOffset(0)] public int Type;
    [FieldOffset(8)] public QfValueUnion As;
}

[StructLayout(LayoutKind.Explicit, Size = 56)]
public unsafe struct QfNode
{
    [FieldOffset(0)] public byte Kind;
    [FieldOffset(1)] public byte Op;
    [FieldOffset(2)] public ushort Flags;
    [FieldOffset(4)] public int A;
    [FieldOffset(8)] public int B;
    [FieldOffset(12)] public int Argc;
    [FieldOffset(16)] public QfValue Lit;
    [FieldOffset(40)] public QfStr Name;
}
