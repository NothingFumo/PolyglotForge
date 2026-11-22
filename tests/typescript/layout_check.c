#include <stdio.h>
#include "qf_layout.h"
#include "qf_keywords.h"

int main(void) {
    if (sizeof(qf_str) != 16 || sizeof(qf_value) != 24 || sizeof(qf_node) != 56) {
        printf("布局漂移\n");
        return 1;
    }
    if (sizeof(qf_col) != 24 || sizeof(qf_schema) != 16 ||
        sizeof(qf_batch) != 16 || sizeof(qf_result) != 32) {
        printf("表/批次/结果布局漂移\n");
        return 1;
    }
    if (sizeof(qf_diag) != 48 || offsetof(qf_diag, hint) != 32) {
        printf("诊断布局漂移\n");
        return 1;
    }
    if (sizeof(qf_ast) != 104 || offsetof(qf_ast, select_names) != 32 ||
        offsetof(qf_ast, from_table) != 40 || offsetof(qf_ast, limit) != 96) {
        printf("计划布局漂移\n");
        return 1;
    }
    if (QF_N_LIT != 1 || QF_OP_EQ != 1 || QF_AGG_COUNT != 1 || QF_T_TEXT != 3) {
        printf("常量漂移\n");
        return 1;
    }
    if (QF_OP_NAME[QF_OP_GE][0] != '>' || QF_AGG_NAME[QF_AGG_AVG][0] != 'a') {
        printf("展示名漂移\n");
        return 1;
    }
    printf("typescript codegen OK: value=%d node=%d op_ge=%s\n",
           (int)sizeof(qf_value), (int)sizeof(qf_node), QF_OP_NAME[QF_OP_GE]);
    return 0;
}
