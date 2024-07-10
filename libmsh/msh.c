#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wordexp.h>

#include <asm/prctl.h>        /* Definition of ARCH_* constants */
#include <sys/syscall.h>      /* Definition of SYS_* constants */
#include <unistd.h>


#include "msh.h"
#include "spinlock.h"

#define BOLDBLUE "\033[1m\033[34m"
#define BLUE     "\033[34m"
#define RESET    "\033[0m"

#define PRINT(str, ...) fprintf(stderr, BOLDBLUE "[MSH] " RESET str, ##__VA_ARGS__)

#define err_check(err_cond, ...)                                               \
    if (err_cond) {                                                            \
        PRINT("%s:%d: ", __FILE__, __LINE__);                        \
        PRINT(__VA_ARGS__);                                          \
        exit(1);                                                               \
    }

// 0-th index of lists is reserved for primary thread
#define push_back_to_list(list, size, item, pos)                               \
    for (int i = 1; i < size; i++) {                                           \
        if (list[i] == -1) {                                                   \
            list[i] = item;                                                    \
            pos     = i;                                                       \
            break;                                                             \
        }                                                                      \
    }

#define pop_back_from_list(list, size, item, pos)                              \
    for (int i = size - 1; i > 0; i--) {                                       \
        if (list[i] != -1) {                                                   \
            item    = list[i];                                                 \
            list[i] = -1;                                                      \
            pos     = i;                                                       \
            break;                                                             \
        }                                                                      \
    }

#define CAS(ptr, old_val, new_val)                                             \
    __sync_bool_compare_and_swap(ptr, old_val, new_val)

// Static allocation of per Application Tables
static struct coroutine_ctx      SCAV_TABLE[MAX_ACTIVE_CRT_NUM_PER_APPLICATION];
static struct primary_thread_ctx PRIM_TABLE[MAX_THREAD_NUM];
spinlock                         scav_table_lock;
spinlock                         prim_table_lock;

static __thread char *gsbase = 0;
#define tl_yield_ctx_at(pos)                                                   \
    (struct yield_ctx *) (gsbase + sizeof(struct yield_ctx) * pos)


#define tl_get_crt_idx_at(pos)     (PRIM_TABLE[msh_tid].idx_list[pos])
#define tl_set_crt_id_at(pos, val) (PRIM_TABLE[msh_tid].idx_list[pos] = val)

static __thread int   cur_crt_pos = 0;
static __thread int   msh_tid     = -1;
static bool           dummy;
static __thread bool *msh_reallocatable   = &dummy;
static __thread bool *msh_reallocated     = &dummy;
static _Atomic int    finished_scav_cnt   = 0;
static int            max_scav_per_thread = 1;

// Scavenger pool management
static char      scav_pool_path[256];
static FILE     *scav_pool_fp    = NULL;
static const int MAX_SCV_CMD_LEN = 256;

// jmp_buf for scheduler
jmp_buf cleanup_jmpbuf[MAX_THREAD_NUM];

static void tl_sched_next_crt(int ret_crt_pos);
static int  allocate_scav(struct primary_thread_ctx *ctx);

#pragma GCC push_options
#pragma GCC optimize("O0")
void
ret_to_scheduler(void) {
    int ret_crt_pos;
    __asm__ volatile(
        "testq $15, %%rsp\n\t"
        "jz _no_adjust\n\t"
        "subq $8, %%rsp\n\t"   // after coroutine returns, rsp might not be
                               // 16-byte alinged (ABI requirement)
        "_no_adjust:\n"
        "movl %%eax, %0\n\t"
        : "=rm"(ret_crt_pos)
        :);
    tl_sched_next_crt(ret_crt_pos);

    // this program point shouldn't be reached
    assert(false);
}
#pragma GCC pop_options

static void
print_prim_ctx(struct primary_thread_ctx *ctx) {
    PRINT("Primary %d: \n", msh_tid);
    PRINT("    idx_list:");
    for (int i = 0; i < MAX_CRT_PER_THREAD + 1; i++) {
        PRINT("%d ", ctx->idx_list[i]);
    }
    PRINT("\n");

    PRINT("    ycs-normal:");
    for (int i = 0; i < MAX_CRT_PER_THREAD + 1; i++) {
        PRINT("%d ", ctx->ycs[i].normal_next);
    }

    PRINT("     ycs-special:");
    for (int i = 0; i < MAX_CRT_PER_THREAD + 1; i++) {
        PRINT("%d ", ctx->ycs[i].special_next);
    }

    PRINT("\n");
    fflush(stderr);
}

static void
parse_command(char *string, int *argc, char ***argv) {
    *argc = 0;
    wordexp_t p;
    wordexp(string, &p, 0);
    *argc = p.we_wordc;
    *argv = p.we_wordv;
}

static void
init_scav_table_entry(struct coroutine_ctx *ctx, char *cmd) {
    cmd[strcspn(cmd, "\n")] = 0;
    parse_command(cmd, &ctx->argc, &ctx->argv);

    void *lib_handle = dlopen(ctx->argv[0], RTLD_NOW | RTLD_DEEPBIND);

    err_check(!lib_handle, "Failed to dlopen file %s %s\n", cmd, dlerror());

    memset(ctx->stack, 0, CRT_STACK_SZ);
    ctx->lib_handle   = lib_handle;
    ctx->entry        = (void (*)(void)) dlsym(lib_handle, "entry");
    ctx->ret_to_sched = (void *) ret_to_scheduler;
    ctx->yc           = NULL;
    ctx->finished     = false;
    err_check(dlerror(), "Failed to dlsym entry in file\n");

    ctx->init   = (void (*)(void)) dlsym(lib_handle, "init");
    ctx->inited = false;
    dlerror();   // reset error. init may not exist, but that's fine
    return;
}

static void
init_prim_table() {
    for (int i = 0; i < MAX_THREAD_NUM; i++) {
        // Cache-line alignment check
        assert((uint64_t) &PRIM_TABLE[i].ycs % 64 == 0);
        for (int j = 0; j < MAX_CRT_PER_THREAD + 1; j++)
            PRIM_TABLE[i].ycs[j].normal_next = -1;
        for (int j = 0; j < MAX_CRT_PER_THREAD + 1; j++)
            PRIM_TABLE[i].idx_list[j] = -1;
        PRIM_TABLE[i].reallocatable = false;
        PRIM_TABLE[i].reallocated   = false;
        PRIM_TABLE[i].scav_num      = 0;
    }
}

static void
init_scav_table(char *local_pool_path) {
    spin_lock(&scav_table_lock);
    scav_pool_fp = fopen(local_pool_path, "r");
    err_check(!scav_pool_fp, "Failed to open file\n");

    for (int i = 0; i < MAX_ACTIVE_CRT_NUM_PER_APPLICATION; i++) {
        SCAV_TABLE[i].lib_handle = NULL;
    }

    char line_buf[MAX_SCV_CMD_LEN];
    int  scav_counts = 0;
    while (fgets(line_buf, MAX_SCV_CMD_LEN, scav_pool_fp)) {
        if (line_buf[0] == '#') {   // # is a comment
            continue;
        }

        err_check(scav_counts > MAX_ACTIVE_CRT_NUM_PER_APPLICATION,
                  "Too many scavengers in the pool\n");

        init_scav_table_entry(&SCAV_TABLE[scav_counts], line_buf);
        scav_counts++;
    }

    PRINT("Scavenger pool has %d entries\n", scav_counts);
    spin_unlock(&scav_table_lock);
}

static void
set_scav_symbol(int scav_idx, int crt_pos) {
    struct coroutine_ctx *ctx = &SCAV_TABLE[scav_idx];

    void *sym = dlsym(ctx->lib_handle, "argc");
    err_check(dlerror(), "Failed to dlsym argc in file\n");
    int *entry_argc = (int *) sym;
    *entry_argc     = ctx->argc;

    sym = dlsym(ctx->lib_handle, "argv");
    err_check(dlerror(), "Failed to dlsym argv in file\n");
    char ***entry_argv = (char ***) sym;
    *entry_argv        = ctx->argv;

    sym = dlsym(ctx->lib_handle, "crt_pos");
    err_check(dlerror(), "Failed to dlsym crt_pos in file\n");
    int *entry_crt_pos = (int *) sym;
    *entry_crt_pos     = crt_pos * sizeof(struct yield_ctx);

    sym = dlsym(ctx->lib_handle, "loop_counter_loc");
    if (dlerror()) {
        PRINT("This scavenger doesn't have loop counter\n");
    } else {
        long *entry_loop_counter_loc = (long *) sym;
        *entry_loop_counter_loc     = offsetof(struct primary_thread_ctx,loop_counter) + (NUM_OF_LINES_FOR_LOOP_COUNTER * 64 * crt_pos); 
        memset(PRIM_TABLE[msh_tid].loop_counter[crt_pos], 0, NUM_OF_LINES_FOR_LOOP_COUNTER * 64);
    }
}

// swap coroutines at a_pos and b_pos in the index list
static void
tl_swap_crts(int a_pos, int b_pos) {
    spin_lock(&scav_table_lock);

    int a_idx = tl_get_crt_idx_at(a_pos);
    int b_idx = tl_get_crt_idx_at(b_pos);
    tl_set_crt_id_at(a_pos, b_idx);
    tl_set_crt_id_at(b_pos, a_idx);

    // swap yc
    struct yield_ctx *a_yc = tl_yield_ctx_at(a_pos);
    struct yield_ctx *b_yc = tl_yield_ctx_at(b_pos);
    struct yield_ctx  tmp;
    memcpy(&tmp, a_yc, sizeof(struct yield_ctx));
    memcpy(a_yc, b_yc, sizeof(struct yield_ctx));
    memcpy(b_yc, &tmp, sizeof(struct yield_ctx));

    SCAV_TABLE[a_idx].yc = b_yc;
    SCAV_TABLE[b_idx].yc = a_yc;

    // update next map
    for (int pos = 0; pos < MAX_CRT_PER_THREAD + 1; pos++) {
        struct yield_ctx *yc = tl_yield_ctx_at(pos);
        if (yc->normal_next == a_pos) {
            yc->normal_next = b_pos;
        } else if (yc->normal_next == b_pos) {
            yc->normal_next = a_pos;
        }
    }

    // swap register set
    struct x86_registers *a_regs = PRIM_TABLE[msh_tid].regs[a_pos];
    struct x86_registers *b_regs = PRIM_TABLE[msh_tid].regs[b_pos];
    struct x86_registers  tmp_regs[MAX_REG_SET_NUM];
    memcpy(tmp_regs, a_regs, sizeof(struct x86_registers) * MAX_REG_SET_NUM);
    memcpy(a_regs, b_regs, sizeof(struct x86_registers) * MAX_REG_SET_NUM);
    memcpy(b_regs, tmp_regs, sizeof(struct x86_registers) * MAX_REG_SET_NUM);

    // swap binary symbol
    // handle only if the symbol is defined at pos
    if (a_idx != -1) {
        void *a_lib_handle = SCAV_TABLE[a_idx].lib_handle;
        assert(a_lib_handle);
        void *a_sym           = dlsym(a_lib_handle, "crt_pos");
        int  *a_entry_crt_pos = (int *) a_sym;
        *a_entry_crt_pos      = b_pos;
    }

    if (b_idx != -1) {
        void *b_lib_handle = SCAV_TABLE[b_idx].lib_handle;
        assert(b_lib_handle);
        void *b_sym           = dlsym(b_lib_handle, "crt_pos");
        int  *b_entry_crt_pos = (int *) b_sym;
        *b_entry_crt_pos      = a_pos;
    }

    spin_unlock(&scav_table_lock);

}

static void
tl_finish_crt(int crt_pos) {
    struct yield_ctx *yc      = tl_yield_ctx_at(crt_pos);
    int               crt_idx = tl_get_crt_idx_at(crt_pos);
    yc->normal_next           = -1;

    if (crt_idx == -1) {
        // primary thread is finished
        return;
    }

    spin_lock(&scav_table_lock);
    tl_set_crt_id_at(crt_pos, -1);
    SCAV_TABLE[crt_idx].yc       = NULL;
    SCAV_TABLE[crt_idx].finished = true;
    dlclose(SCAV_TABLE[crt_idx].lib_handle);
    spin_unlock(&scav_table_lock);
}

static void
set_special_next(struct primary_thread_ctx *ctx) {
    // this function is called every time new scavenger is allocated
    // it keeps the link made through special_next to be a ring

    for (int pos = 1; pos < MAX_CRT_PER_THREAD + 1; pos++) {
        struct yield_ctx *yc = tl_yield_ctx_at(pos);
        if (pos == MAX_CRT_PER_THREAD)
            yc->special_next = 0;

        int idx_next = tl_get_crt_idx_at(pos + 1);
        if (idx_next == -1) {
            yc->special_next = 0;
        } else {
            yc->special_next = (pos + 1) * sizeof(struct yield_ctx);
        }
    }
}

static int
tl_next_available_crt() {
    struct yield_ctx *ycs = PRIM_TABLE[msh_tid].ycs;
    // find if there is any available scavenger coroutine
    for (int pos = 1; pos < MAX_CRT_PER_THREAD + 1; pos++) {
        if (ycs[pos].normal_next != -1)
            return pos;
    }

    // if none, set to primary
    return 0;
}

// TODO: currently we only update normal_next of primary
// we will have to handle special_next of scavenger coroutines later
static void
tl_update_next_map() {
    struct yield_ctx *ycs              = PRIM_TABLE[msh_tid].ycs;
    int               crt_next_to_prim = ycs[0].normal_next;
    if (crt_next_to_prim == -1) {
        // primary thread is finished
        return;
    }

    // next coroutine is finished
    ycs[0].normal_next = sizeof(struct yield_ctx) * tl_next_available_crt();
}

static int
tl_find_unfinished_crt() {
    for (int pos = 1; pos < MAX_CRT_PER_THREAD + 1; pos++) {
        int crt_idx = tl_get_crt_idx_at(pos);
        if (crt_idx != -1) {
            return pos;
        }
    }
    return 0;
}

static void
tl_pack_crts() {
    // if primary is finished, skip
    struct yield_ctx *primary_yc = tl_yield_ctx_at(0);
    if (primary_yc->normal_next == -1) {
        return;
    }

    for (int pos = 1; pos < MAX_CRT_PER_THREAD + 1; pos++) {
        if (tl_get_crt_idx_at(pos) == -1) {
            for (int next_pos = pos + 1; next_pos < MAX_CRT_PER_THREAD + 1;
                 next_pos++) {
                if (tl_get_crt_idx_at(next_pos) != -1) {
                    tl_swap_crts(pos, next_pos);
                    break;
                }
            }
        }
    }

    set_special_next(&PRIM_TABLE[msh_tid]);
}

static void
tl_sched_next_crt(int ret_crt_pos) {
    ret_crt_pos = ret_crt_pos / sizeof(struct yield_ctx); // change to idx (from pointer)
    
    PRINT(
            BOLDBLUE "[%d]              Coroutine at pos %d is finished -- "
                     "Scavenger index = %d\n" RESET,
            msh_tid, ret_crt_pos, tl_get_crt_idx_at(ret_crt_pos));
    if (ret_crt_pos != 0)
        PRINT("FinishedCoroutines: %d \n", ++finished_scav_cnt);
    struct yield_ctx *ret_yield_ctx     = tl_yield_ctx_at(ret_crt_pos);
    struct yield_ctx *primary_yield_ctx = tl_yield_ctx_at(0);

    uint64_t sp;
    uint64_t ip;
    short    next = ret_yield_ctx->normal_next / sizeof(struct yield_ctx);
    // print_prim_ctx(&PRIM_TABLE[msh_tid]);
    // PRINT("[%d] next of retcrt = %d\n", msh_tid, next);
    tl_finish_crt(ret_crt_pos);
    tl_update_next_map();
    tl_pack_crts();

    // print_prim_ctx(&PRIM_TABLE[msh_tid]);
    if (primary_yield_ctx->normal_next != -1) {   // primary is not finished
        allocate_scav(&PRIM_TABLE[msh_tid]);      // get new one if needed
        tl_update_next_map();
    }

    // prefetch yield context
    __asm__ volatile("prefetcht0 %%gs:0\n\t" : :);

    if (primary_yield_ctx->normal_next == -1) {
        // primary is finished
        // we are running cleanup routine
        cur_crt_pos = tl_find_unfinished_crt();
        if (cur_crt_pos == 0) {
            longjmp(cleanup_jmpbuf[msh_tid], 0);
        }
        struct yield_ctx *yc = tl_yield_ctx_at(cur_crt_pos);
        sp                   = yc->sp;
        ip                   = yc->ip;
    } else {
        cur_crt_pos          = next;
        struct yield_ctx *yc = tl_yield_ctx_at(cur_crt_pos);
        sp                   = yc->sp;
        ip                   = yc->ip;
    }

    PRINT("[%d] Coroutine %d is scheduled %p %p\n", msh_tid,
            cur_crt_pos, (void *) sp, (void *) ip);
    __asm__ volatile("movq %1, %%rsp\n\t"
                     "jmp  *%0\n\t"
                     :
                     : "m"(ip), "mr"(sp));
    // should never reach here
    assert(false);
}

// The design of this function is undecided.
// This static cap version is tentative.
static int
need_more_scav(struct primary_thread_ctx *ctx) {
    int counts = 0;
    for (int i = 1; i < MAX_CRT_PER_THREAD + 1; i++) {
        if (ctx->idx_list[i] != -1) {
            counts++;
        }
    }

    if (counts < max_scav_per_thread) {
        return max_scav_per_thread - counts;
    } else {
        return 0;
    }
}

static int
realloc_scav(struct primary_thread_ctx *from, struct primary_thread_ctx *to,
             int demands) {
    int ret_counts = 0;
    while (ret_counts < demands) {
        int crt_idx_to_realloc, from_pos = -1, to_pos = -1;
        pop_back_from_list(from->idx_list, MAX_CRT_PER_THREAD + 1,
                           crt_idx_to_realloc,
                           from_pos);   // pos = idx in idx_list
        if (from_pos == -1)
            break;   // no more scavengers in *from

        push_back_to_list(to->idx_list, MAX_CRT_PER_THREAD + 1,
                          crt_idx_to_realloc, to_pos);
        if (to_pos == -1) {
            // no more space, rollback and return
            push_back_to_list(from->idx_list, MAX_CRT_PER_THREAD + 1,
                              crt_idx_to_realloc, from_pos);
            return ret_counts;
        }

        // no rollback from this point
        // yc update
        struct yield_ctx *from_yc = from->ycs + from_pos;
        struct yield_ctx *to_yc   = to->ycs + to_pos;
        to_yc->sp                 = from_yc->sp;
        to_yc->ip                 = from_yc->ip;
        to_yc->normal_next        = 0;

        from_yc->normal_next = -1;
        tl_update_next_map();
        to->ycs[0].normal_next = sizeof(struct yield_ctx);

        // TODO: handle special next

        // regset update
        struct x86_registers *from_regs = from->regs[from_pos];
        struct x86_registers *to_regs   = to->regs[to_pos];
        memcpy(to_regs, from_regs,
               sizeof(struct x86_registers) * MAX_REG_SET_NUM);

        // binary symbol update
        void *lib_handle    = SCAV_TABLE[crt_idx_to_realloc].lib_handle;
        void *sym           = dlsym(lib_handle, "crt_pos");
        int  *entry_crt_pos = (int *) sym;
        *entry_crt_pos      = to_pos;

        // finalize
        ret_counts++;
        SCAV_TABLE[crt_idx_to_realloc].yc = to_yc;
        __asm__ __volatile__(
            "" ::
                : "memory");   // prevent compiler from reordering
        from->reallocated = true;

        int from_tid = ((uint64_t) from - (uint64_t) PRIM_TABLE) /
                       sizeof(struct primary_thread_ctx);
        int to_tid = ((uint64_t) to - (uint64_t) PRIM_TABLE) /
                     sizeof(struct primary_thread_ctx);
        PRINT("Scav %d is reallocated [%d] --> [%d] %d %d %d %d\n",
                crt_idx_to_realloc, from_tid, to_tid, from->idx_list[1],
                from->idx_list[2], from->idx_list[3], from->idx_list[4]);
    }

    return ret_counts;
}

static int
_allocate_scav(struct primary_thread_ctx *ctx, int demands) {
    int ret_counts = 0;

    // Check if there is reallocatable scavenger
    // Note for concurrency: this is the only place where primary threads may
    // access the context of other primary threads. The access must be protected
    // by "reallocatable" flag.
    for (int tid = 0; tid < MAX_THREAD_NUM; tid++) {
        struct primary_thread_ctx *t = &PRIM_TABLE[tid];
        if (CAS(&t->reallocatable, true, false)) {
            // reallocation for t begins -- t will busy-wait until it's done
            int cnt = realloc_scav(t /*from*/, ctx /*to*/, demands);
            __asm__ __volatile__(
                "" ::
                    : "memory");       // prevent compiler from reordering
            t->reallocatable = true;   // unlock
            ret_counts += cnt;
            if (ret_counts >= demands)
                return ret_counts;
        }
    }

    // check scav pool
    for (int idx = 0; idx < MAX_ACTIVE_CRT_NUM_PER_APPLICATION; idx++) {
        if (!SCAV_TABLE[idx].lib_handle)   // empty entry
            continue;

        if (SCAV_TABLE[idx].yc ||
            SCAV_TABLE[idx].finished)   // already allocated
            continue;

        // allocate idx-th scavenger to the primary
        int pos = -1;
        push_back_to_list(ctx->idx_list, MAX_CRT_PER_THREAD + 1, idx, pos);
        if (pos == -1)
            break;   // no more space

        ctx->ycs[pos].sp          = (uint64_t) &SCAV_TABLE[idx].ret_to_sched;
        ctx->ycs[pos].ip          = (uint64_t) SCAV_TABLE[idx].entry;
        ctx->ycs[pos].normal_next = 0;

        set_scav_symbol(idx, pos);

        SCAV_TABLE[idx].yc = &ctx->ycs[pos];

        ret_counts++;
        if (ret_counts >= demands)
            return ret_counts;
    }

    return ret_counts;
}

static void
call_init_for_new_scav(struct primary_thread_ctx *ctx) {
    for (int pos = 1; pos < MAX_CRT_PER_THREAD + 1; pos++) {
        int crt_idx = tl_get_crt_idx_at(pos);

        if (crt_idx == -1)
            continue;

        struct coroutine_ctx *crt_ctx = &SCAV_TABLE[crt_idx];
        if (crt_ctx->inited)
            continue;

        // a scavenger is assinged uniquely to this primary at this moment, so
        // no need to think about concurrency
        if (crt_ctx->init) {
            struct yield_ctx *yc = tl_yield_ctx_at(pos);

            // yc of primary is not set at this moment, so make crts jump to itself while init
            uint64_t sp_tmp = yc->sp;
            uint64_t ip_tmp = yc->ip;
            short normal_tmp   = yc->normal_next; 
            short special_tmp  = yc->special_next;
            yc->normal_next = pos * sizeof(struct yield_ctx); // set to itself temporarily
            yc->special_next = pos * sizeof(struct yield_ctx); // set to itself temporarily
            
            crt_ctx->init(); 
            crt_ctx->inited = true;
            
            yc->sp = sp_tmp;
            yc->ip = ip_tmp;
            yc->normal_next = normal_tmp;
            yc->special_next = special_tmp;
        }
    }
}

static int
allocate_scav(struct primary_thread_ctx *ctx) {
    int counts = need_more_scav(ctx);
    if (counts == 0)
        return 0;

    spin_lock(&scav_table_lock);
    int ret = _allocate_scav(ctx, counts);
    spin_unlock(&scav_table_lock);

    // scav init -- just call it!!
    call_init_for_new_scav(ctx);
    set_special_next(ctx);
    return ret;
}

static void
init_primary_ctx(struct primary_thread_ctx *ctx, int counts) {
    // sp/ip will be initialized at the first yield
    ctx->ycs[0].normal_next = counts > 0 ? sizeof(struct yield_ctx) : 0;
    // speical_next shouldn't be used in primary
    ctx->scav_num      = counts;
    ctx->reallocatable = 0;
    ctx->reallocated   = 0;
    msh_reallocatable  = &ctx->reallocatable;
    msh_reallocated    = &ctx->reallocated;
}

int
msh_init(int max_scav) {

    int fd = open("/dev/cpu_dma_latency", O_WRONLY);
    if (fd < 0) {
        perror("open /dev/cpu_dma_latency");
        // keep going without setting c-state
        // return 1;
    }
    int l = 0;
    if (write(fd, &l, sizeof(l)) != sizeof(l)) {
        perror("write to /dev/cpu_dma_latency");
        // return 1;
    }

    PRINT("\n\n MSH Init\n");

    // print offset information
    PRINT("    offset to ycs: %lu\n",
            offsetof(struct primary_thread_ctx, ycs));
    PRINT("    offset to regs: %lu\n",
            offsetof(struct primary_thread_ctx, regs));
    PRINT("    offset to idx_list: %lu\n",
            offsetof(struct primary_thread_ctx, idx_list));
    PRINT("    offset to regset: %lu\n",
            offsetof(struct primary_thread_ctx, regs[0]));
        
    PRINT("    size of x86_registers: %lu\n",
            sizeof(struct x86_registers));
    PRINT("    size of regset: %lu\n",
            sizeof(struct x86_registers) * MAX_REG_SET_NUM);
    PRINT("    size of yield context: %lu\n",
            sizeof(struct yield_ctx));
    PRINT("       offset of sp: %lu\n",
            offsetof(struct yield_ctx, sp));
    PRINT("       offset of ip: %lu\n",
            offsetof(struct yield_ctx, ip));
    PRINT("       offset of next: %lu\n",
            offsetof(struct yield_ctx, normal_next));

    PRINT("Warning: this number must match the value in binary "
                    "instrumentation\n");
    max_scav_per_thread = max_scav;

    init_prim_table();

    char *path = getenv("MSH_SCAV_POOL_PATH");
    if (path == NULL) {
        fprintf(stderr,
                "Scavenger pool path should be given in MSH_SCAV_POOL_PATH\n");
        return -1;
    }

    strcpy(scav_pool_path, path);
    init_scav_table(scav_pool_path);

    PRINT("MSH is successfully initiated\n");
    return 0;
}

// identify unallocated scavengers and allocated them
int
msh_alloc_ctx(int tid) {
    // set per-thread variables
    msh_tid     = tid;   // PRIM_TABLE[tid] is yours from now on
    cur_crt_pos = 0;     // current coroutine = primary

    if (tid >= MAX_THREAD_NUM) {
        PRINT("libmsh: thread number exceeds the limit %d\n", tid);
        return -1;
    }

    // use ctx, not tid if possible
    struct primary_thread_ctx *ctx = &PRIM_TABLE[tid];

    syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long *) ctx);
    gsbase = (char *) ctx;
    //_writegsbase_u64(
    //    (uint64_t) ctx);   // set gs. this MUST be done before allocate_scav
                           // because allocate_scav depends on gs

    int cnt = allocate_scav(ctx);

    init_primary_ctx(ctx, cnt);

    PRINT("Primary thread %d has %d scavengers\n", tid, cnt);
    //print_prim_ctx(ctx);

    return 0;
}

int
msh_cleanup() {
    if (getenv("SKIP_CLEANUP")) {
        fflush(stderr);
        fflush(stdout);
        return 0;
    }

    PRINT("Cleanup thread %d\n", msh_tid);
    // setup cleanup
    // primary->next = 0 will be done in tl_sched_next_crt
    for (int pos = 1; pos < MAX_CRT_PER_THREAD + 1; pos++) {
        struct yield_ctx *yc = tl_yield_ctx_at(pos);
        if (yc->normal_next != -1) {
            // set it to itself so that it runs to completion
            yc->normal_next  = pos * sizeof(struct yield_ctx);
            yc->special_next = pos * sizeof(struct yield_ctx);
        }
    }

    // finish remaining scavengers using scheduler code
    setjmp(cleanup_jmpbuf[msh_tid]);
    int unfinished_crt_pos = tl_find_unfinished_crt();
    if (unfinished_crt_pos == 0) {
        fprintf(
            stderr,
            BOLDBLUE
            "[%d]             ***Primary thread [%d] is finished***\n" RESET,
            msh_tid, msh_tid);
        struct yield_ctx *yc =
            tl_yield_ctx_at(0);   // reset yc[0].next = 0 to avoid error
        yc->normal_next = 0;   // do this just in case there is an insrumented
                               // code after cleanup
        return 0;
    }

    tl_sched_next_crt(0);
    assert(false);   // shouldn't reach here
}

void
msh_enter_blockable_call() {
    *msh_reallocatable = true;
}

void
msh_exit_blockable_call() {
    while (!CAS(msh_reallocatable, true, false)) {
        // someone's trying to reallocate scavs
        // busy wait
    }

    __asm__ __volatile__("" ::: "memory");   // prevent compiler from reordering
    if (*msh_reallocated) {
        struct primary_thread_ctx *t = &PRIM_TABLE[msh_tid];
        // slow-path
        PRINT("[%d] my scavs are reallocated...\n", msh_tid);
        allocate_scav(t);
        tl_update_next_map();
        t->reallocated = false;
    }
}
