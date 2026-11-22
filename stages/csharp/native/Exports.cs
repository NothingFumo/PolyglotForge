// 跨语言导出：只有这三个函数，签名严格对齐 contracts/qf.h。
//
//   qf_err qf_cs_compile(const qf_host*, const qf_ast*, int32_t root,
//                        const qf_schema*, qf_pred**, qf_diag*);
//   qf_err qf_cs_eval(qf_pred*, const qf_batch*, uint8_t* keep, qf_diag*);
//   void   qf_cs_free(qf_pred*);
//
// qf_pred 的实际载荷是一个 GCHandle（指向 CompiledPredicate），
// 句柄指针同时就是 qf_pred* 的取值，qf_cs_free 负责释放。
using System;
using System.Runtime.InteropServices;

namespace QfCs;

internal static unsafe class Diag
{
    public static void Zero(QfDiag* d)
    {
        if (d != null) *d = default;
    }

    public static int Fail(QfDiag* d, QfHost* host, int code, string message)
    {
        if (d == null) return code;
        d->Code = code;
        d->Line = 0;
        d->Col = 0;
        d->Message = Str(host, message);
        d->Hint = default;
        return code;
    }

    /// <summary>
    /// 诊断文本优先交给宿主驻留（arena 生命周期由宿主决定）。
    /// qf_cs_eval 的签名里没有 host，这种情形改为本地分配：错误路径只有一次，
    /// 且写出的 qf_str 必须在调用返回后仍然可读，不能指向托管内存。
    /// </summary>
    private static QfStr Str(QfHost* host, string message)
    {
        if (string.IsNullOrEmpty(message)) return default;

        byte[] bytes = System.Text.Encoding.UTF8.GetBytes(message);
        fixed (byte* src = bytes)
        {
            uint len = (uint)bytes.Length;
            if (host != null && host->Str != null) return host->Str(host->Ctx, src, len);

            byte* copy = (byte*)NativeMemory.Alloc(len);
            Buffer.MemoryCopy(src, copy, len, len);
            return new QfStr { Ptr = copy, Len = len };
        }
    }
}

internal static unsafe class Exports
{
    [UnmanagedCallersOnly(EntryPoint = "qf_cs_compile")]
    public static int Compile(QfHost* host, QfAst* ast, int root, QfSchema* table, void** outPred, QfDiag* diag)
    {
        Diag.Zero(diag);
        if (outPred == null) return QfErr.Arg;
        *outPred = null;
        if (host == null) return Diag.Fail(diag, null, QfErr.Arg, "宿主回调缺失");
        if (ast == null) return Diag.Fail(diag, host, QfErr.Arg, "抽象语法树为空");
        if (table == null) return Diag.Fail(diag, host, QfErr.Arg, "表定义为空");

        try
        {
            CompiledPredicate pred = PredicateCompiler.Compile(ast, table, root, host);
            GCHandle handle = GCHandle.Alloc(pred, GCHandleType.Normal);
            *outPred = (void*)GCHandle.ToIntPtr(handle);
            return QfErr.Ok;
        }
        catch (PredicateError e)
        {
            return Diag.Fail(diag, host, QfErr.Type, e.Message);
        }
        catch (Exception e)
        {
            return Diag.Fail(diag, host, QfErr.Panic, "编译谓词时发生未预期错误：" + e.GetType().Name);
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "qf_cs_eval")]
    public static int Eval(void* pred, QfBatch* batch, byte* keep, QfDiag* diag)
    {
        Diag.Zero(diag);
        if (pred == null) return Diag.Fail(diag, null, QfErr.Arg, "谓词句柄为空");
        if (batch == null) return Diag.Fail(diag, null, QfErr.Arg, "批次为空");

        CompiledPredicate p;
        try
        {
            GCHandle handle = GCHandle.FromIntPtr((IntPtr)pred);
            if (!handle.IsAllocated || handle.Target is not CompiledPredicate cp) throw new InvalidOperationException();
            p = cp;
        }
        catch (Exception)
        {
            return Diag.Fail(diag, null, QfErr.Arg, "谓词句柄无效");
        }

        uint rows = batch->Rows;
        uint cols = batch->Cols;
        if (rows == 0) return QfErr.Ok;

        if (keep == null) return Diag.Fail(diag, p.Host, QfErr.Arg, "保留掩码为空");
        if (batch->Values == null) return Diag.Fail(diag, p.Host, QfErr.Arg, "批次缺少值数组");
        if (cols == 0) return Diag.Fail(diag, p.Host, QfErr.Arg, "批次没有列");
        if (cols > (uint)int.MaxValue) return Diag.Fail(diag, p.Host, QfErr.Arg, "批次列数超出范围");
        if (p.ColumnsNeeded > (int)cols)
            return Diag.Fail(diag, p.Host, QfErr.Arg, "批次列数少于谓词引用的列");

        try
        {
            int n = (int)cols;
            var ints = new long[n];
            var dbls = new double[n];
            var texts = new string?[n];
            var nulls = new byte[n];

            for (uint r = 0; r < rows; ++r)
            {
                long baseIdx = (long)r * cols;
                for (int c = 0; c < n; ++c)
                {
                    ints[c] = 0;
                    dbls[c] = 0.0;
                    texts[c] = null;
                    nulls[c] = 0;

                    QfValue v = batch->Values[baseIdx + c];
                    switch (v.Type)
                    {
                    case QfT.Int: ints[c] = v.I; break;
                    case QfT.Double: dbls[c] = v.D; break;
                    case QfT.Text: texts[c] = Abi.Text(v.S.Ptr, v.S.Len); break;
                    default: nulls[c] = 1; break;
                    }
                }

                keep[r] = p.Run(ints, dbls, texts, nulls) ? (byte)1 : (byte)0;
            }
            return QfErr.Ok;
        }
        catch (Exception e)
        {
            return Diag.Fail(diag, p.Host, QfErr.Exec, "求值谓词时发生未预期错误：" + e.GetType().Name);
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "qf_cs_eval_values")]
    public static unsafe int EvalValues(
        QfHost* host, QfAst* ast, int node, QfSchema* table, QfBatch* batch,
        delegate* unmanaged<byte*, QfStr*, QfValue*, uint, QfValue*, QfDiag*, int> udf,
        byte* udfCtx, QfValue* outValues, QfDiag* diag)
    {
        Diag.Zero(diag);
        if (host == null || ast == null || table == null || batch == null || outValues == null)
        {
            return Diag.Fail(diag, host, QfErr.Arg, "求值参数为空");
        }
        if ((uint)node >= ast->NodeCount)
        {
            return Diag.Fail(diag, host, QfErr.Arg, "表达式节点下标越界");
        }

        try
        {
            new ValueEvaluator(ast, batch, host, diag, udf, udfCtx).Run(node, outValues);
            return QfErr.Ok;
        }
        catch (EvalError e)
        {
            // UDF 回调可能已经写入具体原因（例如 Python 异常），不要覆盖。
            if (diag != null && diag->Code != QfErr.Ok) return e.Code;
            return Diag.Fail(diag, host, e.Code, e.Message);
        }
        catch (Exception e)
        {
            return Diag.Fail(diag, host, QfErr.Panic, "求值表达式时发生未预期错误：" + e.GetType().Name);
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "qf_cs_free")]
    public static void Free(void* pred)
    {
        if (pred == null) return;
        try
        {
            GCHandle handle = GCHandle.FromIntPtr((IntPtr)pred);
            if (handle.IsAllocated) handle.Free();
        }
        catch (Exception)
        {
            // 契约里 free 没有错误码，句柄失效时只能静默返回。
        }
    }
}
