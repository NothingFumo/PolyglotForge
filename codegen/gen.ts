/**
 * 从 schema/qf.ts 生成各语言绑定。
 * 运行：bun codegen/gen.ts
 */
import { writeFileSync } from 'node:fs';
import {
  ABI_VERSION, AGG, ERR, FLAGS, KEYWORDS, LAYOUT, NODE_KIND, OP, OP_NAMES, AGG_NAMES, TYPE,
} from '../schema/qf.ts';

const HEADER = '/* 由 schema/qf.ts + codegen/gen.ts 生成，勿手改 */\n';
const write = (path: string, body: string) => writeFileSync(path, HEADER + body, 'utf8');

const enumC = (prefix: string, entries: Record<string, number>, type = '') => {
  const body = Object.entries(entries)
    .map(([k, v]) => `    ${prefix}_${k} = ${v}`)
    .join(',\n');
  return type
    ? `typedef enum {\n${body}\n} ${type};`
    : `enum {\n${body}\n};`;
};

// ---------------- C ----------------
const cLayout = `
#ifndef QF_LAYOUT_H
#define QF_LAYOUT_H
#include <stdint.h>

#define QF_ABI_VERSION ${ABI_VERSION}

typedef int32_t qf_err;
${enumC('QF_ERR', ERR)}

typedef int32_t qf_type;
${Object.entries(TYPE).map(([k, v]) => `enum { QF_T_${k} = ${v} };`).join('\n')}

${enumC('QF_N', NODE_KIND, 'qf_node_kind')}

${enumC('QF_OP', OP, 'qf_op')}

${enumC('QF_AGG', AGG, 'qf_agg')}

enum {
    QF_F_TYPE_SHIFT = ${FLAGS.TYPE_SHIFT},
    QF_F_TYPE_MASK = ${FLAGS.TYPE_MASK},
    QF_F_DESC = ${FLAGS.DESC},
    QF_F_RESOLVED = ${FLAGS.RESOLVED}
};

typedef struct qf_str { const char* ptr; uint32_t len; } qf_str;

typedef struct qf_value {
    qf_type type;
    union { int64_t i; double d; qf_str s; } as;
} qf_value;

typedef struct qf_node {
    uint8_t kind;
    uint8_t op;
    uint16_t flags;
    int32_t a;
    int32_t b;
    int32_t argc;
    qf_value lit;
    qf_str name;
} qf_node;

#if defined(__cplusplus)
#define QF_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define QF_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#define QF_STATIC_ASSERT(cond, msg)
#endif

${Object.entries(LAYOUT).map(([name, l]) => `QF_STATIC_ASSERT(sizeof(${name}) == ${l.size}, "${name} 布局漂移");`).join('\n')}

#endif /* QF_LAYOUT_H */
`;

// ---------------- C# ----------------
const csLayout = `
using System.Runtime.InteropServices;

namespace Qf;

public static class Abi
{
    public const int Version = ${ABI_VERSION};
}

public enum QfErr : int
{
${Object.entries(ERR).map(([k, v]) => `    ${k[0] + k.slice(1).toLowerCase()} = ${v},`).join('\n')}
}

public enum QfType : int
{
${Object.entries(TYPE).map(([k, v]) => `    ${k[0] + k.slice(1).toLowerCase()} = ${v},`).join('\n')}
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
`;

// ---------------- Python ----------------
const pyLayout = `
import ctypes


class QfStr(ctypes.Structure):
    _fields_ = [("ptr", ctypes.c_char_p), ("len", ctypes.c_uint32)]


class QfValueUnion(ctypes.Union):
    _fields_ = [("i", ctypes.c_int64), ("d", ctypes.c_double), ("s", QfStr)]


class QfValue(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int32), ("as_", QfValueUnion)]


class QfNode(ctypes.Structure):
    _fields_ = [
        ("kind", ctypes.c_uint8),
        ("op", ctypes.c_uint8),
        ("flags", ctypes.c_uint16),
        ("a", ctypes.c_int32),
        ("b", ctypes.c_int32),
        ("argc", ctypes.c_int32),
        ("lit", QfValue),
        ("name", QfStr),
    ]


ABI_VERSION = ${ABI_VERSION}
QF_OK = ${ERR.OK}
QF_ERR_PARSE = ${ERR.PARSE}
QF_ERR_TYPE = ${ERR.TYPE}
QF_ERR_UDF = ${ERR.UDF}

QF_T_NULL, QF_T_INT, QF_T_DOUBLE, QF_T_TEXT = ${TYPE.NULL}, ${TYPE.INT}, ${TYPE.DOUBLE}, ${TYPE.TEXT}

QF_NODE_KIND = ${JSON.stringify(NODE_KIND)}
QF_OP = ${JSON.stringify(OP)}
QF_AGG = ${JSON.stringify(AGG)}
`;

// ---------------- C: 关键字与展示名 ----------------
const cKeywords = `
#ifndef QF_KEYWORDS_H
#define QF_KEYWORDS_H

#define QF_KEYWORDS(X) ${KEYWORDS.map((k) => `X("${k}")`).join(' ')}

static const char* const QF_OP_NAME[] = {
${Array.from({ length: 16 }, (_, i) => `    ${JSON.stringify(OP_NAMES[i] ?? '?')}`).join(',\n')}
};

static const char* const QF_AGG_NAME[] = {
    "?", "count", "sum", "avg", "min", "max"
};

#endif /* QF_KEYWORDS_H */
`;

write('gen/qf_layout.h', cLayout);
write('gen/qf_layout.cs', csLayout);
write('gen/qf_layout.py', pyLayout);
write('gen/qf_keywords.h', cKeywords);
console.log('generated:', ['gen/qf_layout.h', 'gen/qf_layout.cs', 'gen/qf_layout.py', 'gen/qf_keywords.h'].join(', '));
console.log('agg names:', Object.values(AGG_NAMES).join(','));
