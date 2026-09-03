#include <stdbool.h>
#include <stdint.h>

#include "cases2/qwen3_14b_decode.h"

#define TASK_OSTD 2

#define get_ipc_reg_0() 0
#define get_ipc_reg_1() 1
#define get_ipc_reg_2() 2
#define get_ipc_reg_3() 3
#define get_ipc_reg_4() 4
#define get_ipc_reg_5() 5
#define get_ipc_reg_6() 6
#define get_ipc_reg_7() 7

#define reset_ipc_reg_6() 0
#define reset_ipc_reg_2() 0

bool slot_free[TASK_OSTD] = {true, true};

#define INVALID_TASK_ID 0xFFFFFFFF

inline void publish(uint32_t counter_id) {
    // TODO
}

inline void subscribe(uint32_t counter_id, uint32_t expect_value, uint8_t scb_id) {
    // TODO
}

inline void fake_kernel() {
    // TODO
}

#define EARLY_DISPATCH 1

void resolve_dep(uint32_t task_id) {
    uint32_t succ_id;
    uint32_t succ_cnt;
    uint32_t idx;

    succ_cnt = (uint32_t)task_suc_cnt[task_id];
    if (succ_cnt <= 0) return;
    
    idx = task_suc_idx[task_id];
    for (uint32_t k = idx; k < (idx + succ_cnt); k++) {
        succ_id = task_successors[k];
        task_pre_cnt[succ_id]--;
        if (task_pre_cnt[succ_id] < EARLY_DISPATCH) {
            task_type_t type = task_type[succ_id];
        }
    }
}

void on_task_done(uint64_t task_id) {
    publish(task_id);
}

inline void check_ostd_1() {
    uint64_t task_id = get_ipc_reg_4();
    
    if (task_id != INVALID_TASK_ID)
    {
        uint64_t predecessor_id = get_ipc_reg_5();
        if (predecessor_id > 0)
        {
            subscribe(predecessor_id, 1, 6);
            reset_ipc_reg_6();
        }
        uint16_t cond = get_ipc_scb_6();

        if (cond > 0)
        {
            uint64_t context_addr = get_ipc_reg_6();
            fake_kernel();
            reset_ipc_reg_4();
            slot_free[1] = true;
            on_task_done(task_id);
        }
    }
}

inline void check_ostd_0() {
    uint64_t task_id = get_ipc_reg_0();
    if (task_id != INVALID_TASK_ID)
    {
        uint64_t predecessor_id = get_ipc_reg_1();
        if (predecessor_id > 0)
        {
            subscribe(predecessor_id, 1, 0);
            reset_ipc_reg_2();
        }
        uint16_t cond = get_ipc_scb_0();

        if (cond > 0)
        {
            uint64_t context_addr = get_ipc_reg_2();
            fake_kernel();
            reset_ipc_reg_0();
            slot_free[0] = true;
            on_task_done(task_id);
        }
    }
}



void run(bool isCube) {
    uint16_t core_id = (uint16_t)get_coreid();
    task_type_t type = isCube ? CUBE : VECTOR;
    while (true) {
        if (slot_free[0])
        {
            uint64_t task_id = INVALID_TASK_ID;
            set_ipc_reg_0(task_id);
            queue_pop(core_id, type, &task_id);
            uint64_t predecessor_id;
            set_ipc_reg_2(predecessor_id);
            slot_free[0] = false;
        } else {
            check_ostd_0();
        }

        if (slot_free[1])
        {
            uint64_t task_id = INVALID_TASK_ID;
            queue_pop(core_id, type, &task_id);
            set_ipc_reg_4(task_id);
            uint64_t predecessor_id = get_ipc_reg_1();
            set_ipc_reg_6(predecessor_id);
            slot_free[1] = false;
        } else {
            check_ostd_1();
        }
    }
}