# 由 schema/qf.ts + codegen/gen.ts 生成，勿手改

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


class QfCol(ctypes.Structure):
    _fields_ = [("name", QfStr), ("type", ctypes.c_int32)]


class QfSchema(ctypes.Structure):
    _fields_ = [("count", ctypes.c_uint32), ("cols", ctypes.POINTER(QfCol))]


class QfBatch(ctypes.Structure):
    _fields_ = [
        ("rows", ctypes.c_uint32),
        ("cols", ctypes.c_uint32),
        ("values", ctypes.POINTER(QfValue)),
    ]


class QfResult(ctypes.Structure):
    _fields_ = [
        ("schema", QfSchema),
        ("row_count", ctypes.c_uint64),
        ("values", ctypes.POINTER(QfValue)),
    ]


class QfDiag(ctypes.Structure):
    _fields_ = [
        ("code", ctypes.c_int32),
        ("line", ctypes.c_uint32),
        ("col", ctypes.c_uint32),
        ("message", QfStr),
        ("hint", QfStr),
    ]


class QfAst(ctypes.Structure):
    _fields_ = [
        ("nodes", ctypes.POINTER(QfNode)),
        ("node_count", ctypes.c_uint32),
        ("select", ctypes.POINTER(ctypes.c_int32)),
        ("select_count", ctypes.c_uint32),
        ("select_names", ctypes.POINTER(QfStr)),
        ("from_table", QfStr),
        ("where_root", ctypes.c_int32),
        ("group_by", ctypes.POINTER(ctypes.c_int32)),
        ("group_count", ctypes.c_uint32),
        ("order_by", ctypes.POINTER(ctypes.c_int32)),
        ("order_count", ctypes.c_uint32),
        ("limit", ctypes.c_int64),
    ]


ABI_VERSION = 3
QF_OK = 0
QF_ERR_PARSE = 3
QF_ERR_TYPE = 4
QF_ERR_UDF = 7

QF_T_NULL, QF_T_INT, QF_T_DOUBLE, QF_T_TEXT = 0, 1, 2, 3

QF_NODE_KIND = {"LIT":1,"COL":2,"BIN":3,"UNARY":4,"CALL":5,"AGG":6,"STAR":7}
QF_OP = {"NONE":0,"EQ":1,"NE":2,"LT":3,"LE":4,"GT":5,"GE":6,"AND":7,"OR":8,"NOT":9,"ADD":10,"SUB":11,"MUL":12,"DIV":13,"MOD":14,"NEG":15}
QF_AGG = {"COUNT":1,"SUM":2,"AVG":3,"MIN":4,"MAX":5}

assert ctypes.sizeof(QfStr) == 16
assert ctypes.sizeof(QfValue) == 24
assert ctypes.sizeof(QfNode) == 56
assert ctypes.sizeof(QfCol) == 24
assert ctypes.sizeof(QfSchema) == 16
assert ctypes.sizeof(QfBatch) == 16
assert ctypes.sizeof(QfResult) == 32
assert ctypes.sizeof(QfDiag) == 48
assert ctypes.sizeof(QfAst) == 104
assert QfAst.select_names.offset == 32
assert QfAst.limit.offset == 96
