/* 由 schema/qf.ts + codegen/gen.ts 生成，勿手改 */

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


ABI_VERSION = 1
QF_OK = 0
QF_ERR_PARSE = 3
QF_ERR_TYPE = 4
QF_ERR_UDF = 7

QF_T_NULL, QF_T_INT, QF_T_DOUBLE, QF_T_TEXT = 0, 1, 2, 3

QF_NODE_KIND = {"LIT":1,"COL":2,"BIN":3,"UNARY":4,"CALL":5,"AGG":6,"STAR":7}
QF_OP = {"NONE":0,"EQ":1,"NE":2,"LT":3,"LE":4,"GT":5,"GE":6,"AND":7,"OR":8,"NOT":9,"ADD":10,"SUB":11,"MUL":12,"DIV":13,"MOD":14,"NEG":15}
QF_AGG = {"COUNT":1,"SUM":2,"AVG":3,"MIN":4,"MAX":5}
