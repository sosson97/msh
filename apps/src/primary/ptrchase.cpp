/* check if /proc/sys/vm/nr_hugepages > 0 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <linux/mman.h>
#include <sched.h>
#include <sys/mman.h>
#include <pthread.h>

#include <time.h>

/**
 * system configurations
 * netsys-c27
 */
#define CORE_BASE     28   // make sure each core in different physical core
#define CORE_NUM      1
#define JOBS_PER_CORE 1
#define CACHE_LINE_SZ 64
#define NODE_ALIGN    64
#define FREQ_MHZ      2100

#define ARR_LEN        1024 * 64
#define REPS   10

#define RDTSC_TO_NS(x) ((x) *1000 / FREQ_MHZ)

#define THREAD_NUM 1
// #define POINTER_CHASE
// #define SHUFFLE

#pragma GCC push_options
#pragma GCC optimize("O0")

static inline uint64_t
start64_rdtsc() {
    uint64_t t;
    __asm__ volatile("lfence\n\t"
                     "rdtsc\n\t"
                     "shl $32, %%rdx\n\t"
                     "or %%rdx, %0\n\t"
                     "lfence"
                     : "=a"(t)
                     :
                     : "rdx", "memory", "cc");
    return t;
}

static inline uint64_t
stop64_rdtsc() {
    uint64_t t;
    __asm__ volatile("rdtscp\n\t"
                     "shl $32, %%rdx\n\t"
                     "or %%rdx, %0\n\t"
                     "lfence"
                     : "=a"(t)
                     :
                     : "rcx", "rdx", "memory", "cc");
    return t;
}

static inline uint64_t
start64_ts() {
    return start64_rdtsc();
}

static inline uint64_t
stop64_ts() {
    return stop64_rdtsc();
}
#pragma GCC pop_options

#define handle_error(msg)                                                      \
    do {                                                                       \
        perror(msg);                                                           \
        exit(EXIT_FAILURE);                                                    \
    } while (0)

struct node {
    struct node *next;
    char         pad[NODE_ALIGN - sizeof(struct node *)];
};

struct status {
    int          cur_tid;
    struct node *last_pos;

#ifndef POINTER_CHASE
    uint64_t last_idx;
#endif
};

struct thread_arg {
    int      tid;
    uint64_t arr_len;
    uint64_t rep_per_task;
    int      is_tq;
};

struct node *data_arr = NULL;

static int seed_randomizer = 0;
void
gen_cycle(int *out, uint64_t out_len) {
    uint64_t idx, dst_idx;
    int      tmp;
    srand((unsigned) time(NULL) + seed_randomizer++);

    int *perm = (int *) malloc(sizeof(int) * out_len);

    for (idx = 0; idx < out_len; idx++)
        perm[idx] = idx;

    for (idx = 0; idx < out_len; idx++) {
        dst_idx       = rand() % out_len;
        tmp           = perm[idx];
        perm[idx]     = perm[dst_idx];
        perm[dst_idx] = tmp;
    }

    for (idx = 0; idx < out_len; idx++)
        out[perm[idx]] = perm[(idx + 1) % out_len];
}

void
init_run(uint64_t arr_len) {
    int *cycle = (int *) malloc(sizeof(int) * arr_len);

    if (data_arr == NULL) {
        data_arr = (struct node *) mmap(
            NULL, sizeof(struct node) * arr_len * CORE_NUM * JOBS_PER_CORE,
            PROT_READ | PROT_WRITE,
            MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB | MAP_HUGE_2MB, -1, 0);
        if (data_arr == MAP_FAILED)
            handle_error("mmap");
        for (int job_id = 0; job_id < CORE_NUM * JOBS_PER_CORE; job_id++) {
            int cur = 0;
            gen_cycle(cycle, arr_len);
            do {
                data_arr[job_id * arr_len + cur].next =
                    &data_arr[job_id * arr_len + cycle[cur]];
                cur = cycle[cur];
            } while (cur != 0);
        }
    }

    free(cycle);
}

uint64_t run_time[THREAD_NUM];
void *
ptrchase_tq(void *arg) {
    int tid = ((struct thread_arg *) arg)->tid;
    uint64_t rep_per_task = ((struct thread_arg *) arg)->rep_per_task;
    uint64_t start, end;

    struct node *cur  = data_arr;
    uint64_t     reps = 0;

    start = start64_ts();
    while (reps++ < rep_per_task) {
        // here shuld be the coroutine yield point
        cur = cur->next;
    }
    end = stop64_ts();

    run_time[tid] = end - start;
    
    fprintf(stderr, "[%d] ptrchase finishes\n", tid);
    fprintf(stderr, "[%d] AMAT: %lu ns\n", tid, RDTSC_TO_NS(run_time[tid] / REPS / (rep_per_task / REPS)));

    return NULL;
}

void
run(uint64_t arr_len) {
    thread_arg targ[THREAD_NUM];

    init_run(arr_len);

    pthread_t t[THREAD_NUM];
    for (int i = 0; i < THREAD_NUM; i++) {
        targ[i].rep_per_task = arr_len * REPS;
        targ[i].tid = i;
        if (pthread_create(&t[i], NULL, ptrchase_tq, &targ[i]))
            handle_error("pthread_create");
    }

    for (int i = 0; i < THREAD_NUM; i++)
        pthread_join(t[i], NULL); 

    //ptrchase_tq(&targ);

    return;
}

int main(int argc, char **argv) {
    uint64_t arr_len = 0;
    if (argc > 1)
        arr_len = atoi(argv[1]);
    else
        arr_len = ARR_LEN;

    fprintf(stderr, "ptrchase %lu %d\n", arr_len, REPS);

    run(arr_len);

    return 0;
}
