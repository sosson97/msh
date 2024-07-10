#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

static uint64_t total_reps = 100 * 1000 * 1000;
static uint64_t gsum = 0;

#define FREQ_MHZ       2100
#define RDTSC_TO_NS(x) ((x) *1000 / FREQ_MHZ)

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

uint64_t run_time = 0;
int comp_len = 10;
void
compute() {
    uint64_t start, end;
    uint64_t rep = 0;

    start = start64_ts();
    while (++rep < total_reps) {
        // Here should be coroutine yield point
        int x = 0;
        for (int i = 0; i < comp_len; i++) {
            x += i;
        }

        gsum += x;
    }
    end      = stop64_ts();
    run_time = end - start;
}

#pragma GCC push_options
#pragma GCC optimize("O0")
extern "C" {
int    crt_pos      = 0;
int    argc         = 0;
char **argv         = 0;
}

extern "C" int
entry(void) {
    if (argc > 1) {
        comp_len = atoi(argv[1]);
        total_reps = std::stoull(argv[2]);
    }

    fprintf(stderr, "compute starts -- crt_pos = %d\n", crt_pos);

    compute();

    fprintf(stderr, "compute finishes %d\n", crt_pos);
    fprintf(stderr, "per task time: %lu ns\n",
            RDTSC_TO_NS(run_time) / total_reps);
    
    // coroutine context is designed to return to
    // "ret_to_scheduler"

    // set %rax = crt_pos
    return crt_pos;
}
#pragma GCC pop_options
