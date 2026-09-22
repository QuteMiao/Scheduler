/* Execute the generated adjacency arrays without touching device MMIO. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include HEADER

#if HEADER_FORMAT == 1
#define PRE_COUNT(i) pre_cnt[i]
#define SUC_COUNT(i) suc_cnt[i]
#define SUC_INDEX(i) suc_idx[i]
#define SUCCESSOR(i) successors[i]
#define DURATION(i) total_duration[i]
#define TYPE(i) total_type[i]
#else
#define PRE_COUNT(i) atomic_load(&task_pre_cnt[i])
#define SUC_COUNT(i) task_suc_cnt[i]
#define SUC_INDEX(i) task_suc_idx[i]
#define SUCCESSOR(i) task_successors[i]
#define DURATION(i) task_duration[i]
#define TYPE(i) task_type[i]
#endif

int main(void) {
    int *left = calloc((size_t)total_task_cnt, sizeof(int));
    int *queue = calloc((size_t)total_task_cnt, sizeof(int));
    assert(left && queue);
    int head = 0, tail = 0;
    long pre_edges = 0, suc_edges = 0;
    for (int i = 0; i < total_task_cnt; ++i) {
        assert(total_task_id[i] == (uint32_t)i);
        assert(TYPE(i) == 0 || TYPE(i) == 1);
        assert(DURATION(i) >= 0);
        left[i] = PRE_COUNT(i);
        assert(left[i] >= 0 && SUC_COUNT(i) >= 0);
        pre_edges += left[i];
        suc_edges += SUC_COUNT(i);
        int parent_xor = 0;
        for (int j = 0; j < left[i]; ++j) {
            int p = predecessors[pre_idx[i] + j];
            assert(p >= 0 && p < total_task_cnt && p != i);
            if (j) assert(predecessors[pre_idx[i] + j - 1] < p);
            parent_xor ^= p;
            int matches = 0;
            for (int k = 0; k < SUC_COUNT(p); ++k)
                matches += SUCCESSOR(SUC_INDEX(p) + k) == i;
            assert(matches == 1);
        }
#if HEADER_FORMAT == 2
        assert(parent_xor == task_pre_xor[i]);
#endif
        if (!left[i]) queue[tail++] = i;
    }
    assert(pre_edges == suc_edges);
    while (head < tail) {
        int task = queue[head++];
        for (int i = 0; i < SUC_COUNT(task); ++i) {
            int child = SUCCESSOR(SUC_INDEX(task) + i);
            assert(child >= 0 && child < total_task_cnt && left[child] > 0);
            if (--left[child] == 0) queue[tail++] = child;
        }
    }
    assert(tail == total_task_cnt);
    printf("PASS: %d nodes, %ld edges, all dependencies resolved\n", tail, pre_edges);
    free(left);
    free(queue);
    return 0;
}
