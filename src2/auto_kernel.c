#include <stdbool.h>
#include <stdint.h>

#include "include2/hw_queue.h"

uint16_t core_id = 0;

#define INVALID_TASK_ID 0xFFFFFFFFFFFFFFFF

inline void fake_kernel() {
    // TODO
}


void run(bool isCube) {
    core_id = (uint16_t)get_coreid();
    task_type_t type = isCube ? CUBE : VECTOR;

    while (true) {
        uint64_t task_id = INVALID_TASK_ID;
        if(queue_pop(core_id, type, &task_id)) {
            fake_kernel();
            while(complete_queue_push(core_id, task_id)) {
                // wait
            }
        }
    }
}