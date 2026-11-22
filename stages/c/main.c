/*
 * CLI 与流水线编排。顺序：解析 → 扫描（推断表结构）→ 计划 → 过滤 → 执行 → 输出。
 * 每个阶段失败都会带上稳定错误码与位置，便于端到端测试断言。
 */
#include "qf.h"
#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

qf_host qf_host_make(qf_arena* a);

static const char* err_name(qf_err e) {
    switch (e) {
    case QF_ERR_OK: return "ok";
    case QF_ERR_ARG: return "arg";
    case QF_ERR_IO: return "io";
    case QF_ERR_PARSE: return "parse";
    case QF_ERR_TYPE: return "type";
    case QF_ERR_PLAN: return "plan";
    case QF_ERR_EXEC: return "exec";
    case QF_ERR_UDF: return "udf";
    case QF_ERR_PANIC: return "panic";
    default: return "unsupported";
    }
}

static const char* err_default(qf_err e) {
    switch (e) {
    case QF_ERR_ARG: return "参数无效";
    case QF_ERR_IO: return "输入输出失败";
    case QF_ERR_PARSE: return "解析失败";
    case QF_ERR_TYPE: return "类型错误";
    case QF_ERR_PLAN: return "计划构建失败";
    case QF_ERR_EXEC: return "执行失败";
    case QF_ERR_UDF: return "UDF 执行失败";
    case QF_ERR_PANIC: return "内部错误";
    case QF_ERR_UNSUPPORTED: return "不支持的功能";
    default: return "未知错误";
    }
}

/* 任何非零退出都必须有可读诊断：阶段未填 diag 时用返回码兜底。 */
static void report_error(const qf_diag* d, qf_err rc) {
    qf_err code = (d && d->code != QF_ERR_OK) ? d->code : rc;
    if (code == QF_ERR_OK) code = QF_ERR_PANIC;
    fprintf(stderr, "error[%s]: ", err_name(code));
    if (d && d->message.ptr && d->message.len) {
        fprintf(stderr, "%.*s", (int)d->message.len, d->message.ptr);
    } else {
        fprintf(stderr, "%s", err_default(code));
    }
    fprintf(stderr, "\n");
    if (d && d->hint.ptr && d->hint.len) {
        fprintf(stderr, "hint: %.*s\n", (int)d->hint.len, d->hint.ptr);
    }
    if (d && d->line) fprintf(stderr, "  at %u:%u\n", d->line, d->col);
}

static qf_str S(const char* s) {
    qf_str r;
    r.ptr = s;
    r.len = (uint32_t)strlen(s);
    return r;
}

static void print_raw(const qf_result* r) {
    for (uint32_t c = 0; c < r->schema.count; ++c) {
        printf("%.*s%s", (int)r->schema.cols[c].name.len,
               r->schema.cols[c].name.ptr ? r->schema.cols[c].name.ptr : "",
               c + 1 == r->schema.count ? "\n" : "\t");
    }
    for (uint64_t i = 0; i < r->row_count; ++i) {
        for (uint32_t c = 0; c < r->schema.count; ++c) {
            qf_value v = r->values[i * r->schema.count + c];
            switch (v.type) {
            case QF_T_INT: printf("%lld", (long long)v.as.i); break;
            case QF_T_DOUBLE: printf("%g", v.as.d); break;
            case QF_T_TEXT: printf("%.*s", (int)v.as.s.len, v.as.s.ptr ? v.as.s.ptr : ""); break;
            default: break; /* 空值打印为空串，与 TSV 惯例一致 */
            }
            printf(c + 1 == r->schema.count ? "\n" : "\t");
        }
    }
}

typedef struct js_filter_ctx {
    const qf_host*   host;
    qf_str           expr;
    const qf_schema* table;
} js_filter_ctx;

static qf_err js_filter_fn(void* ctx, const qf_batch* batch, uint8_t* keep, qf_diag* diag) {
    js_filter_ctx* f = (js_filter_ctx*)ctx;
    return qf_js_where_named(f->host, f->expr, f->table, batch, keep, diag);
}

typedef struct cs_filter_ctx {
    qf_pred* pred;
} cs_filter_ctx;

static qf_err cs_filter_fn(void* ctx, const qf_batch* batch, uint8_t* keep, qf_diag* diag) {
    return qf_cs_eval(((cs_filter_ctx*)ctx)->pred, batch, keep, diag);
}

typedef struct run_env {
    const qf_host* host;
    qf_arena* arena;
    const char* table_path;
    const char* where_js;
    const char* dsl;
    int explain;
    int raw;
    int quiet;
    int shards;
} run_env;

typedef struct run_out {
    qf_result result;
    qf_str explain;
    int explain_only;
} run_out;

static qf_err exec_query(run_env* env, qf_str sql, run_out* out, qf_diag* diag) {
    qf_ast* ast = NULL;
    qf_err rc = env->dsl ? qf_dsl_build(env->host, S(env->dsl), &ast, diag)
                         : qf_sql_parse(env->host, sql, &ast, diag);
    if (rc != QF_ERR_OK) return rc;

    qf_schema table;
    memset(&table, 0, sizeof table);
    qf_batch batch;
    memset(&batch, 0, sizeof batch);
    rc = qf_csv_scan(env->host, S(env->table_path), &table, (uint32_t)env->shards, &batch, diag);
    if (rc != QF_ERR_OK) return rc;

    qf_schema out_schema;
    memset(&out_schema, 0, sizeof out_schema);
    rc = qf_plan_build(env->host, ast, &table, &out_schema, diag);
    if (rc != QF_ERR_OK) return rc;

    if (env->explain) {
        rc = qf_engine_explain(env->host, env->arena, ast, &table, &out->explain, diag);
        out->explain_only = 1;
        return rc;
    }

    qf_filter_fn filter = NULL;
    js_filter_ctx jsctx;
    cs_filter_ctx csctx;
    memset(&csctx, 0, sizeof csctx);

    if (env->where_js) {
        /* --where 是“替代编译谓词”的过滤器：SQL 里有没有 WHERE 都要装上，
           否则 where_root < 0 的查询会静默跳过沙箱。 */
        jsctx.host = env->host;
        jsctx.expr = S(env->where_js);
        jsctx.table = &table;
        filter = js_filter_fn;
    } else if (ast->where_root >= 0) {
        qf_pred* pred = NULL;
        qf_diag pdiag;
        memset(&pdiag, 0, sizeof pdiag);
        rc = qf_cs_compile(env->host, ast, ast->where_root, &table, &pred, &pdiag);
        if (rc == QF_ERR_OK) {
            csctx.pred = pred;
            filter = cs_filter_fn;
        } else if (rc == QF_ERR_UNSUPPORTED) {
            filter = NULL; /* 由引擎自带的求值器兜底 */
            rc = QF_ERR_OK;
        } else {
            if (diag) *diag = pdiag;
            return rc;
        }
    }

    void* fctx = env->where_js ? (void*)&jsctx : (void*)&csctx;
    rc = qf_engine_execute(env->host, env->arena, ast, &table, &out_schema, &batch,
                           filter, fctx, &out->result, diag);
    if (csctx.pred) qf_cs_free(csctx.pred);
    return rc;
}

static void emit(run_env* env, const run_out* out) {
    if (env->quiet) return;
    if (out->explain_only) {
        printf("%.*s", (int)out->explain.len, out->explain.ptr ? out->explain.ptr : "");
        return;
    }
    if (env->raw) {
        print_raw(&out->result);
        return;
    }
    qf_str formatted = {NULL, 0};
    qf_diag diag;
    memset(&diag, 0, sizeof diag);
    if (qf_js_format(env->host, &out->result, &formatted, &diag) == QF_ERR_OK) {
        printf("%.*s", (int)formatted.len, formatted.ptr ? formatted.ptr : "");
    } else {
        print_raw(&out->result);
    }
}

typedef struct batch_ctx {
    run_env* env;
    const char* const* parts; /* 与 Kotlin 回调收到的 sql 指针比对，定位查询槽位 */
    uint32_t nparts;
    qf_arena* arenas[64];     /* 失败查询的 arena：diag 指向其中，批处理结束统一释放 */
} batch_ctx;

static qf_err batch_run_one(void* ctx, const char* sql, uint64_t* rows_out, qf_diag* diag) {
    batch_ctx* b = (batch_ctx*)ctx;
    run_env* env = b->env;

    uint32_t slot = b->nparts;
    for (uint32_t i = 0; i < b->nparts; ++i) {
        if (b->parts[i] == sql) {
            slot = i;
            break;
        }
    }

    /* 每条查询独占 arena：回调在 Kotlin 协程里并发执行，共享 arena 会交出重叠内存。 */
    qf_arena* arena = qf_arena_new(1u << 16);
    if (!arena) return QF_ERR_EXEC;
    qf_host host = qf_host_make(arena);

    run_env local = *env;
    local.host = &host;
    local.arena = arena;

    run_out out;
    memset(&out, 0, sizeof out);
    qf_err rc = exec_query(&local, S(sql), &out, diag);
    if (rc == QF_ERR_OK) {
        *rows_out = out.result.row_count;
        qf_arena_free(arena);
    } else if (slot < b->nparts) {
        /* diag.message 指向该 arena，Kotlin 侧要读完才返回，故延迟到批处理结束释放。 */
        b->arenas[slot] = arena;
    }
    return rc;
}

static void usage(void) {
    printf(
        "queryforge —— 九语言合一的查询引擎\n"
        "\n"
        "  queryforge <table.csv> \"<sql>\" [选项]\n"
        "\n"
        "选项\n"
        "  --explain            打印计划，不执行\n"
        "  --raw                输出 TSV（跳过 JS 表格渲染）\n"
        "  --where <js-expr>    用 JS 沙箱过滤（替代编译谓词）\n"
        "  --dsl <文本>         用 Kotlin DSL 代替 SQL\n"
        "  --plugin <file.py>   加载 UDF 插件\n"
        "  --shards <n>         扫描分片数（默认 8）\n"
        "  --batch \"<q1>;<q2>\"  用 Kotlin 协程并发执行多条查询\n"
        "  --version            打印 ABI 版本\n");
}

int main(int argc, char** argv) {
#ifdef _WIN32
    /* 输出必须与对照实现逐字节一致：文本模式下 \n 会被翻译成 \r\n。 */
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 2) {
        usage();
        return 64;
    }
    if (strcmp(argv[1], "--version") == 0) {
        printf("queryforge abi %u\n", qf_abi_version());
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    run_env env;
    memset(&env, 0, sizeof env);
    env.shards = 8;

    const char* table = NULL;
    const char* sql = NULL;
    const char* batch = NULL;
    const char* plugin = NULL;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (strcmp(a, "--explain") == 0) env.explain = 1;
        else if (strcmp(a, "--raw") == 0) env.raw = 1;
        else if (strcmp(a, "--where") == 0 && i + 1 < argc) env.where_js = argv[++i];
        else if (strcmp(a, "--dsl") == 0 && i + 1 < argc) env.dsl = argv[++i];
        else if (strcmp(a, "--plugin") == 0 && i + 1 < argc) plugin = argv[++i];
        else if (strcmp(a, "--shards") == 0 && i + 1 < argc) env.shards = atoi(argv[++i]);
        else if (strcmp(a, "--batch") == 0 && i + 1 < argc) batch = argv[++i];
        else if (a[0] == '-' && a[1] == '-') {
            fprintf(stderr, "未知选项: %s\n", a);
            return 64;
        } else if (!table) table = a;
        else if (!sql) sql = a;
        else {
            fprintf(stderr, "多余参数: %s\n", a);
            return 64;
        }
    }
    if (!table) {
        usage();
        return 64;
    }

    qf_arena* arena = qf_arena_new(1u << 20);
    if (!arena) return 70;
    qf_host host = qf_host_make(arena);
    env.host = &host;
    env.arena = arena;
    env.table_path = table;

    qf_js_init();

    qf_diag diag;
    memset(&diag, 0, sizeof diag);

    if (plugin) {
        qf_err rc = qf_py_load(&host, S(plugin), &diag);
        if (rc != QF_ERR_OK) {
            report_error(&diag, rc);
            qf_arena_free(arena);
            return 65;
        }
    }

    int status = 0;

    if (batch) {
        char* copy = (char*)qf_arena_alloc(arena, strlen(batch) + 1, 1);
        memcpy(copy, batch, strlen(batch) + 1);
        const char* parts[64];
        uint32_t n = 0;
        char* p = copy;
        while (p && *p && n < 64) {
            parts[n++] = p;
            char* semi = strchr(p, ';');
            if (semi) {
                *semi = '\0';
                p = semi + 1;
            } else {
                p = NULL;
            }
        }
        uint64_t rows[64];
        memset(rows, 0, sizeof rows);
        batch_ctx bctx;
        memset(&bctx, 0, sizeof bctx);
        bctx.env = &env;
        bctx.parts = parts;
        bctx.nparts = n;
        env.quiet = 0;
        qf_err rc = qf_kt_batch(&host, batch_run_one, &bctx, parts, n, rows, &diag);
        if (rc != QF_ERR_OK) {
            report_error(&diag, rc);
            status = 65;
        } else {
            for (uint32_t i = 0; i < n; ++i) printf("query %u: %llu rows\n", i + 1, (unsigned long long)rows[i]);
        }
        for (uint32_t i = 0; i < 64; ++i) {
            if (bctx.arenas[i]) qf_arena_free(bctx.arenas[i]);
        }
    } else {
        if (!sql && !env.dsl) {
            usage();
            qf_arena_free(arena);
            return 64;
        }
        run_out out;
        memset(&out, 0, sizeof out);
        /* 只给 --dsl 时没有位置 SQL：S(NULL) 会在 strlen 上崩，这里给空串。 */
        qf_err rc = exec_query(&env, sql ? S(sql) : S(""), &out, &diag);
        if (rc != QF_ERR_OK) {
            report_error(&diag, rc);
            status = 65;
        } else {
            emit(&env, &out);
        }
    }

    qf_py_shutdown();
    qf_arena_free(arena);
    return status;
}
