@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class)

/*
 * C 枚举常量在 cinterop 里是 UInt，而 C 侧的 qf_err / kind / op 都是 32 位整数语义。
 * 转换集中在这里，调用点只用 Int，避免每处散落 toInt()。
 */
package qf.stage

import qf.c.*

internal object Codes {
    val ERR_OK = QF_ERR_OK.toInt()
    val ERR_ARG = QF_ERR_ARG.toInt()
    val ERR_PARSE = QF_ERR_PARSE.toInt()
    val ERR_PANIC = QF_ERR_PANIC.toInt()

    val T_NULL = QF_T_NULL.toInt()
    val T_INT = QF_T_INT.toInt()
    val T_DOUBLE = QF_T_DOUBLE.toInt()
    val T_TEXT = QF_T_TEXT.toInt()

    val N_LIT = QF_N_LIT.toInt()
    val N_COL = QF_N_COL.toInt()
    val N_BIN = QF_N_BIN.toInt()
    val N_UNARY = QF_N_UNARY.toInt()
    val N_CALL = QF_N_CALL.toInt()
    val N_AGG = QF_N_AGG.toInt()
    val N_STAR = QF_N_STAR.toInt()

    val OP_EQ = QF_OP_EQ.toInt()
    val OP_NE = QF_OP_NE.toInt()
    val OP_LT = QF_OP_LT.toInt()
    val OP_LE = QF_OP_LE.toInt()
    val OP_GT = QF_OP_GT.toInt()
    val OP_GE = QF_OP_GE.toInt()
    val OP_AND = QF_OP_AND.toInt()
    val OP_OR = QF_OP_OR.toInt()
    val OP_NOT = QF_OP_NOT.toInt()
    val OP_ADD = QF_OP_ADD.toInt()
    val OP_SUB = QF_OP_SUB.toInt()
    val OP_MUL = QF_OP_MUL.toInt()
    val OP_DIV = QF_OP_DIV.toInt()
    val OP_MOD = QF_OP_MOD.toInt()
    val OP_NEG = QF_OP_NEG.toInt()

    val AGG_COUNT = QF_AGG_COUNT.toInt()
    val AGG_SUM = QF_AGG_SUM.toInt()
    val AGG_AVG = QF_AGG_AVG.toInt()
    val AGG_MIN = QF_AGG_MIN.toInt()
    val AGG_MAX = QF_AGG_MAX.toInt()

    val F_DESC = QF_F_DESC.toInt()
}
