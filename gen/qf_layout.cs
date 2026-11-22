/* 由 schema/qf.ts + codegen/gen.ts 生成，勿手改 */

// 由 schema/qf.ts + codegen/gen.ts 生成，勿手改
using System.Runtime.InteropServices;

namespace QfCs;

[StructLayout(LayoutKind.Explicit, Size = 16)]
internal unsafe struct QfStr
{
    [FieldOffset(0)] public byte* Ptr;
    [FieldOffset(8)] public uint Len;
}

[StructLayout(LayoutKind.Explicit, Size = 24)]
internal unsafe struct QfValue
{
    [FieldOffset(0)] public int Type;
    [FieldOffset(8)] public long I;
    [FieldOffset(8)] public double D;
    [FieldOffset(8)] public QfStr S;
}

[StructLayout(LayoutKind.Explicit, Size = 56)]
internal unsafe struct QfNode
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

[StructLayout(LayoutKind.Explicit, Size = 24)]
internal unsafe struct QfCol
{
    [FieldOffset(0)] public QfStr Name;
    [FieldOffset(16)] public int Type;
}

[StructLayout(LayoutKind.Explicit, Size = 16)]
internal unsafe struct QfSchema
{
    [FieldOffset(0)] public uint Count;
    [FieldOffset(8)] public QfCol* Cols;
}

[StructLayout(LayoutKind.Explicit, Size = 16)]
internal unsafe struct QfBatch
{
    [FieldOffset(0)] public uint Rows;
    [FieldOffset(4)] public uint Cols;
    [FieldOffset(8)] public QfValue* Values;
}

[StructLayout(LayoutKind.Explicit, Size = 32)]
internal unsafe struct QfResult
{
    [FieldOffset(0)] public QfSchema Schema;
    [FieldOffset(16)] public ulong RowCount;
    [FieldOffset(24)] public QfValue* Values;
}

[StructLayout(LayoutKind.Explicit, Size = 48)]
internal unsafe struct QfDiag
{
    [FieldOffset(0)] public int Code;
    [FieldOffset(4)] public uint Line;
    [FieldOffset(8)] public uint Col;
    [FieldOffset(16)] public QfStr Message;
    [FieldOffset(32)] public QfStr Hint;
}

[StructLayout(LayoutKind.Explicit, Size = 104)]
internal unsafe struct QfAst
{
    [FieldOffset(0)] public QfNode* Nodes;
    [FieldOffset(8)] public uint NodeCount;
    [FieldOffset(16)] public int* Select;
    [FieldOffset(24)] public uint SelectCount;
    [FieldOffset(32)] public QfStr* SelectNames;
    [FieldOffset(40)] public QfStr FromTable;
    [FieldOffset(56)] public int WhereRoot;
    [FieldOffset(64)] public int* GroupBy;
    [FieldOffset(72)] public uint GroupCount;
    [FieldOffset(80)] public int* OrderBy;
    [FieldOffset(88)] public uint OrderCount;
    [FieldOffset(96)] public long Limit;
}
