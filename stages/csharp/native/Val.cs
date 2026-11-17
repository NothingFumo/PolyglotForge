// 谓词求值期的值模型与运算符语义。
//
// 只存在于表达式树内部：谓词对外的形态是
//   Func<long[], double[], string?[], byte[], bool>
// 所以这里同时定义列到值的构造，以及三种真值（真/假/未知）的传播规则。
//
// 数值与文本语义对齐 stages/c/engine.c 的 eval：
//   - 空值参与比较/算术一律产出空值；
//   - 整数与浮点互相提升，整数之间优先保持 int64 精度；
//   - 文本比较按 ordinal（契约要求，不跟随区域设置）。
// 布尔运算按 SQL 三值规则：false AND unknown = false，true OR unknown = true；
// 过滤时只有确定为真的行被保留（unknown 与 false 同样丢弃）。
using System;

namespace QfCs;

internal sealed class Val
{
    public const int KNull = 0;
    public const int KInt = 1;
    public const int KDouble = 2;
    public const int KText = 3;

    public static readonly Val Null = new Val(KNull, 0, 0.0, null);
    public static readonly Val True = new Val(KInt, 1, 0.0, null);
    public static readonly Val False = new Val(KInt, 0, 0.0, null);

    public readonly int Kind;
    public readonly long I;
    public readonly double D;
    public readonly string? S;

    private Val(int kind, long i, double d, string? s)
    {
        Kind = kind;
        I = i;
        D = d;
        S = s;
    }

    // 工厂方法：表达式树通过这些 MethodInfo 落地成 Call 节点。
    public static Val Int(long v) => new Val(KInt, v, 0.0, null);
    public static Val Dbl(double v) => new Val(KDouble, 0, v, null);
    public static Val Text(string? s) => s is null ? Null : new Val(KText, 0, 0.0, s);

    public bool IsNum => Kind == KInt || Kind == KDouble;

    public double Num => Kind == KInt ? I : D;

    /// <summary>真值：1 为真、0 为假、-1 为未知。文本不参与布尔运算。</summary>
    private static int Truth(Val v)
    {
        if (v.Kind == KInt) return v.I != 0 ? 1 : 0;
        if (v.Kind == KDouble) return v.D != 0.0 ? 1 : 0;
        return -1;
    }

    /// <summary>过滤语义：只有确定为真才保留该行，未知与假一样被丢弃。</summary>
    public static bool IsTrue(Val v) => Truth(v) == 1;

    public static Val And(Val a, Val b)
    {
        int x = Truth(a), y = Truth(b);
        if (x == 0 || y == 0) return False;
        if (x < 0 || y < 0) return Null;
        return True;
    }

    public static Val Or(Val a, Val b)
    {
        int x = Truth(a), y = Truth(b);
        if (x == 1 || y == 1) return True;
        if (x < 0 || y < 0) return Null;
        return False;
    }

    public static Val Not(Val a)
    {
        int x = Truth(a);
        return x < 0 ? Null : (x == 0 ? True : False);
    }

    public static Val Neg(Val a)
    {
        if (a.Kind == KInt) return Int(unchecked(-a.I));
        if (a.Kind == KDouble) return Dbl(-a.D);
        return Null;
    }

    /// <summary>比较：未知传播，数值按数值比较，文本按 ordinal，类型不可比时产出未知。</summary>
    public static Val Compare(int op, Val a, Val b)
    {
        if (a.Kind == KNull || b.Kind == KNull) return Null;

        int cmp;
        if (a.IsNum && b.IsNum)
        {
            if (a.Kind == KInt && b.Kind == KInt)
            {
                cmp = a.I < b.I ? -1 : (a.I > b.I ? 1 : 0);
            }
            else
            {
                double x = a.Num, y = b.Num;
                cmp = x < y ? -1 : (x > y ? 1 : 0);
            }
        }
        else if (a.Kind == KText && b.Kind == KText)
        {
            cmp = Math.Sign(string.CompareOrdinal(a.S, b.S));
        }
        else
        {
            return Null;
        }

        bool r;
        switch (op)
        {
        case QfOp.Eq: r = cmp == 0; break;
        case QfOp.Ne: r = cmp != 0; break;
        case QfOp.Lt: r = cmp < 0; break;
        case QfOp.Le: r = cmp <= 0; break;
        case QfOp.Gt: r = cmp > 0; break;
        case QfOp.Ge: r = cmp >= 0; break;
        default: return Null;
        }
        return r ? True : False;
    }

    /// <summary>算术：未知传播，整数运算保持 int64，除法一律提升为浮点。</summary>
    public static Val Arith(int op, Val a, Val b)
    {
        if (a.Kind == KNull || b.Kind == KNull) return Null;
        if (!a.IsNum || !b.IsNum) return Null;

        if (a.Kind == KInt && b.Kind == KInt)
        {
            switch (op)
            {
            case QfOp.Add: return Int(unchecked(a.I + b.I));
            case QfOp.Sub: return Int(unchecked(a.I - b.I));
            case QfOp.Mul: return Int(unchecked(a.I * b.I));
            case QfOp.Mod: return b.I == 0 ? Null : Int(a.I % b.I);
            case QfOp.Div: break; // 整数除法也提升成浮点，与 C 引擎一致
            default: return Null;
            }
        }

        double x = a.Num, y = b.Num;
        switch (op)
        {
        case QfOp.Add: return Dbl(x + y);
        case QfOp.Sub: return Dbl(x - y);
        case QfOp.Mul: return Dbl(x * y);
        case QfOp.Div: return y == 0.0 ? Null : Dbl(x / y);
        case QfOp.Mod: return y == 0.0 ? Null : Dbl(x % y);
        default: return Null;
        }
    }
}

/// <summary>编译期的语义错误；导出层统一映射为 QF_ERR_TYPE 并写进 qf_diag。</summary>
internal sealed class PredicateError : Exception
{
    public PredicateError(string message) : base(message)
    {
    }
}
