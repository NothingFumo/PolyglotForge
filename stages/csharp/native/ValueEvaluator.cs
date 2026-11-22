// 通用表达式求值器：把整批行的某个表达式求成 qf_value 数组。
// 投影、分组键与聚合参数都走这里；语义只依赖 Val，不再有第二套实现。
using System;

namespace QfCs;

internal sealed class EvalError : Exception
{
    public int Code { get; }

    public EvalError(int code, string message) : base(message)
    {
        Code = code;
    }
}

internal sealed unsafe class ValueEvaluator
{
    private readonly QfNode* _nodes;
    private readonly int _nodeCount;
    private readonly QfBatch* _batch;
    private readonly QfHost* _host;
    private readonly QfDiag* _diag;
    private readonly delegate* unmanaged<byte*, QfStr*, QfValue*, uint, QfValue*, QfDiag*, int> _udf;
    private readonly byte* _udfCtx;

    public ValueEvaluator(QfAst* ast, QfBatch* batch, QfHost* host, QfDiag* diag,
        delegate* unmanaged<byte*, QfStr*, QfValue*, uint, QfValue*, QfDiag*, int> udf,
        byte* udfCtx)
    {
        if (ast->NodeCount > 0 && ast->Nodes == null)
        {
            throw new EvalError(QfErr.Arg, "AST 节点数组为空");
        }
        // 直接绑定查询期 AST，避免每个投影列都复制一遍节点数组。
        _nodes = ast->Nodes;
        _nodeCount = checked((int)ast->NodeCount);
        _batch = batch;
        _host = host;
        _diag = diag;
        _udf = udf;
        _udfCtx = udfCtx;
    }

    public void Run(int root, QfValue* outValues)
    {
        uint rows = _batch->Rows;
        uint cols = _batch->Cols;
        for (uint r = 0; r < rows; ++r)
        {
            outValues[r] = ToValue(Eval(root, r, cols));
        }
    }

    private Val Eval(int idx, uint row, uint cols)
    {
        if ((uint)idx >= (uint)_nodeCount) throw new EvalError(QfErr.Arg, "表达式节点下标越界");
        QfNode n = _nodes[idx];
        switch (n.Kind)
        {
        case QfNodeKind.Lit:
            return FromValue(n.Lit);
        case QfNodeKind.Col:
            if (n.A < 0 || (uint)n.A >= cols) throw new EvalError(QfErr.Arg, "列下标越界");
            return FromValue(_batch->Values[row * cols + (uint)n.A]);
        case QfNodeKind.Bin:
            {
                Val l = Eval(n.A, row, cols);
                Val r = Eval(n.B, row, cols);
                return n.Op switch
                {
                    QfOp.And => Val.And(l, r),
                    QfOp.Or => Val.Or(l, r),
                    QfOp.Eq or QfOp.Ne or QfOp.Lt or QfOp.Le or QfOp.Gt or QfOp.Ge
                        => Val.Compare(n.Op, l, r),
                    QfOp.Add or QfOp.Sub or QfOp.Mul or QfOp.Div or QfOp.Mod
                        => Val.Arith(n.Op, l, r),
                    _ => throw new EvalError(QfErr.Type, "未知二元运算符"),
                };
            }
        case QfNodeKind.Unary:
            {
                Val v = Eval(n.A, row, cols);
                return n.Op switch
                {
                    QfOp.Not => Val.Not(v),
                    QfOp.Neg => Val.Neg(v),
                    _ => throw new EvalError(QfErr.Type, "未知一元运算符"),
                };
            }
        case QfNodeKind.Call:
            return CallUdf(n, row, cols);
        case QfNodeKind.Agg:
            throw new EvalError(QfErr.Type, "聚合节点不参与表达式求值");
        default:
            throw new EvalError(QfErr.Type, "未知节点类型");
        }
    }

    private Val CallUdf(QfNode n, uint row, uint cols)
    {
        if (n.Argc < 0 || n.Argc > 16) throw new EvalError(QfErr.Udf, "UDF 参数过多");
        if (_udf == null) throw new EvalError(QfErr.Udf, "宿主未提供 UDF 回调");
        var args = new QfValue[n.Argc];
        for (int i = 0; i < n.Argc; ++i) args[i] = ToValue(Eval(n.B + i, row, cols));
        fixed (QfValue* argv = args)
        {
            QfStr name = n.Name;
            QfValue result;
            int rc = _udf(_udfCtx, &name, argv, (uint)n.Argc, &result, _diag);
            if (rc != QfErr.Ok) throw new EvalError(rc, "UDF 执行失败");
            return FromValue(result);
        }
    }

    private static Val FromValue(QfValue v)
    {
        switch (v.Type)
        {
        case QfT.Int: return Val.Int(v.I);
        case QfT.Double: return Val.Dbl(v.D);
        case QfT.Text:
            if (v.S.Ptr == null) return Val.Null;
            return Val.Text(Abi.Text(v.S.Ptr, v.S.Len));
        default: return Val.Null;
        }
    }

    private QfValue ToValue(Val v)
    {
        switch (v.Kind)
        {
        case Val.KInt: return new QfValue { Type = QfT.Int, I = v.I };
        case Val.KDouble: return new QfValue { Type = QfT.Double, D = v.D };
        case Val.KText:
            {
                byte[] bytes = System.Text.Encoding.UTF8.GetBytes(v.S ?? string.Empty);
                fixed (byte* p = bytes)
                {
                    QfStr s = _host->Str(_host->Ctx, p, (uint)bytes.Length);
                    return new QfValue { Type = QfT.Text, S = s };
                }
            }
        default: return new QfValue { Type = QfT.Null };
        }
    }
}
