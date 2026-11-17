// 把 AST 谓词编译成
//   Expression<Func<long[], double[], string?[], byte[], bool>>
//
// 四个参数是同一批行的列式视图：
//   ints   —— QF_T_INT 列的取值
//   dbls   —— QF_T_DOUBLE 列的取值
//   texts  —— QF_T_TEXT 列的取值
//   nulls  —— 空值掩码，非 0 表示该格是空值
// 列引用在树里就是"参数 + 常量下标"，因此同一份编译结果可复用到任意批次，
// 每次求值只需重建每行的列切片。
//
// NativeAOT 下 RuntimeFeature.IsDynamicCodeSupported 为 false，Compile()
// 走解释器（CanCompileToIL=false），语义不变。
using System;
using System.Linq.Expressions;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace QfCs;

internal sealed unsafe class CompiledPredicate
{
    public readonly Func<long[], double[], string?[], byte[], bool> Run;

    /// <summary>谓词引用到的最大列下标 + 1，用于在求值前校验批次列数。</summary>
    public readonly int ColumnsNeeded;

    /// <summary>编译期拿到的宿主，仅在写诊断消息时使用。</summary>
    public readonly QfHost* Host;

    public CompiledPredicate(Func<long[], double[], string?[], byte[], bool> run, int columnsNeeded, QfHost* host)
    {
        Run = run;
        ColumnsNeeded = columnsNeeded;
        Host = host;
    }
}

internal sealed unsafe class PredicateCompiler
{
    private const int MaxDepth = 256;

    private static readonly MethodInfo MAnd = ((Func<Val, Val, Val>)Val.And).Method;
    private static readonly MethodInfo MOr = ((Func<Val, Val, Val>)Val.Or).Method;
    private static readonly MethodInfo MNot = ((Func<Val, Val>)Val.Not).Method;
    private static readonly MethodInfo MNeg = ((Func<Val, Val>)Val.Neg).Method;
    private static readonly MethodInfo MCmp = ((Func<int, Val, Val, Val>)Val.Compare).Method;
    private static readonly MethodInfo MArith = ((Func<int, Val, Val, Val>)Val.Arith).Method;
    private static readonly MethodInfo MInt = ((Func<long, Val>)Val.Int).Method;
    private static readonly MethodInfo MDbl = ((Func<double, Val>)Val.Dbl).Method;
    private static readonly MethodInfo MText = ((Func<string?, Val>)Val.Text).Method;
    private static readonly MethodInfo MIsTrue = ((Func<Val, bool>)Val.IsTrue).Method;

    private readonly QfNode[] _nodes;
    private readonly int[] _colTypes;
    private readonly Expression?[] _memo;

    private readonly ParameterExpression _ints = Expression.Parameter(typeof(long[]), "ints");
    private readonly ParameterExpression _dbls = Expression.Parameter(typeof(double[]), "dbls");
    private readonly ParameterExpression _texts = Expression.Parameter(typeof(string[]), "texts");
    private readonly ParameterExpression _nulls = Expression.Parameter(typeof(byte[]), "nulls");

    private int _maxCol = -1;

    private PredicateCompiler(QfNode[] nodes, int[] colTypes)
    {
        _nodes = nodes;
        _colTypes = colTypes;
        _memo = new Expression?[nodes.Length];
    }

    public static CompiledPredicate Compile(QfAst* ast, QfSchema* table, int root, QfHost* host)
    {
        if (ast->Nodes == null || ast->NodeCount == 0) throw new PredicateError("抽象语法树为空");
        if ((uint)root >= ast->NodeCount) throw new PredicateError("谓词根节点下标越界");

        int count = checked((int)ast->NodeCount);
        var nodes = new QfNode[count];
        for (int i = 0; i < count; ++i) nodes[i] = ast->Nodes[i];

        int cols = checked((int)table->Count);
        if (cols > 0 && table->Cols == null) throw new PredicateError("表定义缺少列数组");

        var colTypes = new int[cols];
        for (int i = 0; i < cols; ++i) colTypes[i] = table->Cols[i].Type;

        var compiler = new PredicateCompiler(nodes, colTypes);
        Expression body = compiler.Build(root, 0);

        var lambda = Expression.Lambda<Func<long[], double[], string?[], byte[], bool>>(
            Expression.Call(MIsTrue, body),
            compiler._ints, compiler._dbls, compiler._texts, compiler._nulls);

        // AOT 无动态代码：显式要求解释执行，避免调用到不可用的 IL 生成路径。
        bool interpret = !RuntimeFeature.IsDynamicCodeSupported;
        Func<long[], double[], string?[], byte[], bool> run = lambda.Compile(interpret);
        return new CompiledPredicate(run, compiler._maxCol + 1, host);
    }

    private Expression Build(int idx, int depth)
    {
        if ((uint)idx >= (uint)_nodes.Length) throw new PredicateError("谓词引用了越界的节点下标");
        Expression? cached = _memo[idx];
        if (cached != null) return cached;
        if (depth > MaxDepth) throw new PredicateError("谓词嵌套层次过深");

        QfNode n = _nodes[idx];
        Expression e;
        switch (n.Kind)
        {
        case QfNodeKind.Lit:
            e = Expression.Constant(Literal(n.Lit), typeof(Val));
            break;

        case QfNodeKind.Col:
            e = Column(n);
            break;

        case QfNodeKind.Bin:
            e = Binary(n, depth);
            break;

        case QfNodeKind.Unary:
            {
                Expression v = Build(n.A, depth + 1);
                e = n.Op switch
                {
                    QfOp.Not => Expression.Call(MNot, v),
                    QfOp.Neg => Expression.Call(MNeg, v),
                    _ => throw new PredicateError("谓词出现未知一元运算符"),
                };
                break;
            }

        case QfNodeKind.Call:
            throw new PredicateError("谓词不支持调用 UDF");

        case QfNodeKind.Agg:
            throw new PredicateError("谓词不支持聚合函数");

        default:
            throw new PredicateError("谓词出现未知节点类型");
        }

        _memo[idx] = e;
        return e;
    }

    private Expression Binary(QfNode n, int depth)
    {
        Expression l = Build(n.A, depth + 1);
        Expression r = Build(n.B, depth + 1);
        switch (n.Op)
        {
        case QfOp.And: return Expression.Call(MAnd, l, r);
        case QfOp.Or: return Expression.Call(MOr, l, r);
        case QfOp.Eq:
        case QfOp.Ne:
        case QfOp.Lt:
        case QfOp.Le:
        case QfOp.Gt:
        case QfOp.Ge:
            return Expression.Call(MCmp, Expression.Constant((int)n.Op), l, r);
        case QfOp.Add:
        case QfOp.Sub:
        case QfOp.Mul:
        case QfOp.Div:
        case QfOp.Mod:
            return Expression.Call(MArith, Expression.Constant((int)n.Op), l, r);
        default:
            throw new PredicateError("谓词出现未知二元运算符");
        }
    }

    private Expression Column(QfNode n)
    {
        int col = n.A;
        if (col < 0 || col >= _colTypes.Length) throw new PredicateError("谓词引用了不存在的列");

        // 计划阶段会在 flags 的高 4 位回填类型；没回填时退回表定义。
        int fromFlags = (n.Flags >> QfFlags.TypeShift) & QfFlags.TypeMask;
        int type = fromFlags != QfT.Null ? fromFlags : _colTypes[col];

        if (col > _maxCol) _maxCol = col;

        Expression index = Expression.Constant(col);
        Expression isNull = Expression.NotEqual(
            Expression.ArrayIndex(_nulls, index),
            Expression.Constant((byte)0));
        Expression notNull;

        switch (type)
        {
        case QfT.Int:
            notNull = Expression.Call(MInt, Expression.ArrayIndex(_ints, index));
            break;
        case QfT.Double:
            notNull = Expression.Call(MDbl, Expression.ArrayIndex(_dbls, index));
            break;
        case QfT.Text:
            notNull = Expression.Call(MText, Expression.ArrayIndex(_texts, index));
            break;
        default:
            return Expression.Constant(Val.Null, typeof(Val));
        }

        return Expression.Condition(isNull, Expression.Constant(Val.Null, typeof(Val)), notNull);
    }

    /// <summary>字面量在编译期就变成常量，故文本会在编译时复制一份托管副本。</summary>
    private static Val Literal(QfValue v)
    {
        switch (v.Type)
        {
        case QfT.Int: return Val.Int(v.I);
        case QfT.Double: return Val.Dbl(v.D);
        case QfT.Text: return Val.Text(Abi.Text(v.S.Ptr, v.S.Len));
        default: return Val.Null;
        }
    }
}
