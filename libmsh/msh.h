#ifndef __MSH_H__
#define __MSH_H__
#include <immintrin.h>
#include <stdint.h>
#include <stdbool.h>

#define CRT_STACK_SZ                       (1 << 20)  // 1MB

// This number are over-provisioned to avoid out-of-resource issue.
#define MAX_THREAD_NUM                     128
#define MAX_ACTIVE_CRT_NUM_PER_APPLICATION 512
#define MAX_CRT_PER_THREAD                 128
#define MAX_REG_SET_NUM                    15 // You may need more regiser set depending on the number of 
#define NUM_OF_LINES_FOR_LOOP_COUNTER      4

// x86 GPR + floating-point registers
struct x86_registers {
    void *rax;
    void *rbx;
    void *rcx;
    void *rdx;
    void *rsi;
    void *rdi;
    void *rbp;
    void *rsp;
    void *r8;
    void *r9;
    void *r10;
    void *r11;
    void *r12;
    void *r13;
    void *r14;
    void *r15;
    void *XMM0[2];
    void *XMM1[2];
    void *XMM2[2];
    void *XMM3[2];
};

// performance critical contexts
// the field order must be unchanged -- binary instrumentator relies on the offset of fields
struct __attribute__((aligned(4), packed)) yield_ctx {
    // We hope the first instance of this struct starts at cache line boundary
    // If so, we can pack three coroutine_ctx per cache line
    uint64_t sp;   // last stack pointer
    uint64_t ip;   // last instruction pointer
    short    special_next;
    short    normal_next;   // normal_next = -1 means the coroutine is finished
};

// Coroutine Context represents a scavenger thread
// the field order must be respected
struct __attribute__((aligned(16), packed)) coroutine_ctx {
    void *lib_handle;            // 0
    void (*entry)(void);         // 8
    char  pad[8];                // 16, padded for stack alignment
    char  stack[CRT_STACK_SZ];   // 24
    void *ret_to_sched;          // this must be at the top of stack
    char  pad2[32];
    // entry arugments
    int               argc;
    char            **argv;
    struct yield_ctx *yc;   // pointer to current yc
    bool               finished;
    void (*init)(void); // scavenger init function
    bool inited;
};

// gs points to primary_thread_ctx of the current thread
struct __attribute__((aligned(64))) primary_thread_ctx {
    struct yield_ctx ycs[MAX_CRT_PER_THREAD + 1];   // should be contiguous, 0
                                                    // is reserved for primary
    char loop_counter[MAX_CRT_PER_THREAD + 1][NUM_OF_LINES_FOR_LOOP_COUNTER * 64];
    struct x86_registers
        regs[MAX_CRT_PER_THREAD + 1]
            [MAX_REG_SET_NUM];   // manipulated by insturmented code
                                 // space for register saving for
                                 // optimizations that is unable to use stack
                                 // - FUR
                                 // - Loop Optimization
    int idx_list[MAX_CRT_PER_THREAD + 1];   // list of scavenger thread index that
                                        // this primary hosts
    int scav_num;
    bool reallocatable;   // whether the scavengers allocated to this primary
                         // can be reallocated to other primaries
    bool reallocated;     // whether the scavengers allocated to this primary
                         // have been reallocated to other primaries
};

/**
 * Interfaces
 * Pthread wrappers use thses calls.
 * WARNING: all functions work in the context of the current thread
 * You should assume that every interface includes the thread context
 * as the first argument, and every function except msh_init() is reentrant.
 */
int msh_init(int max_scav);
int msh_alloc_ctx(int tid);
int msh_cleanup();

void msh_enter_blockable_call();
void msh_exit_blockable_call();

#endif /* __MSH_H__ */
