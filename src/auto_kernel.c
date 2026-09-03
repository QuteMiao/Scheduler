#include <stdbool.h>
#include <stdint.h>

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

inline void publish(uint32_t counter_id) {
    // TODO
}

inline void subscribe(uint32_t counter_id, uint32_t expect_value, uint8_t scb_id) {
    // TODO
}

inline void fake_kernel() {
    // TODO
}

inline void check_ostd_1(uint8_t idx, uint64_t* prev_task_id) {
    uint64_t task_id = get_ipc_reg_4();
    
    if (task_id != prev_task_id[idx])
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
            prev_task_id[idx] = task_id;
            publish(task_id);
        }
    }
}

inline void check_ostd_0(uint8_t idx, uint64_t* prev_task_id) {
    uint64_t task_id = get_ipc_reg_0();
    if (task_id != prev_task_id[idx])
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
            prev_task_id[idx] = task_id;
            publish(task_id);
        }
    }
}

void run(bool isCube) {
    uint64_t prev_task_id[TASK_OSTD] = {0};

    while (true) {
        check_ostd_0(0, prev_task_id);
        check_ostd_1(1, prev_task_id);
    }
}