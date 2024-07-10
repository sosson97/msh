/**
 * This library will be preloaded on the primary's address space
 * Main purpose is
 * 1) overriding some pthread functions
 * 2) declare some global symbols
 *
 * This code assumes C11 standard
 */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "msh.h"

static __thread int msh_thread_id;
static _Atomic int  thread_cnt = 0;

struct msh_start_routine_arg {
    void *(*start_routine)(void *);
    void *arg;
    int   thread_id;
};


// this routine is invoked in the context of the new thread
void *
msh_start_routine(void *arg) {
    msh_thread_id = ((struct msh_start_routine_arg *) arg)->thread_id;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int err       = msh_alloc_ctx(msh_thread_id);
    clock_gettime(CLOCK_MONOTONIC, &end);
    fprintf(stdout, "msh_alloc_time= %ld ms\n", (end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000);

    if (err) {
        fprintf(stderr, "msh_alloc_ctx() failed\n");
        return NULL;
    }

    void *ret =
        ((struct msh_start_routine_arg *) arg)
            ->start_routine(((struct msh_start_routine_arg *) arg)->arg);

    err = msh_cleanup(msh_thread_id);
    if (err) {
        fprintf(stderr, "msh_cleanup() failed\n");
        return NULL;
    }

    return ret;
}

// overriden pthread_create
int (*original_pthread_create)(pthread_t *restrict,
                               const pthread_attr_t *restrict,
                               void *(*) (void *), void *restrict) = NULL;

int
pthread_create(pthread_t *restrict thread, const pthread_attr_t *restrict attr,
               void *(*start_routine)(void *), void *restrict arg) {
    if (thread_cnt == 0) {
        char *max_scav_str = getenv("MAX_SCAV_PER_THREAD");
        int max_scav      = 1;
        if (max_scav_str)
            max_scav = atoi(max_scav_str);

        if (msh_init(max_scav)) {
            fprintf(stderr, "msh_init() failed\n");
            return -1;
        }


        if (!getenv("SKIP_FIRST_THREAD")) {
            int err = msh_alloc_ctx(thread_cnt++);
            if (err) {
                fprintf(stderr, "msh_alloc_ctx() failed\n");
                return -1;
            }
        }
    }

    struct msh_start_routine_arg *msh_arg =
        (struct msh_start_routine_arg *) malloc(
            sizeof(struct msh_start_routine_arg));
    msh_arg->start_routine = start_routine;
    msh_arg->arg           = arg;
    msh_arg->thread_id     = thread_cnt++;
    if (!original_pthread_create)
        original_pthread_create = dlsym(RTLD_NEXT, "pthread_create");

    int ret = original_pthread_create(thread, attr, msh_start_routine,
                                      (void *) msh_arg);
    return ret;
}

/**
 * pthread blockable calls
 */

int (*original_pthread_mutex_lock)(pthread_mutex_t *mutex) = NULL;

int
pthread_mutex_lock(pthread_mutex_t *mutex) {
    msh_enter_blockable_call();
    if (!original_pthread_mutex_lock) {
        original_pthread_mutex_lock = dlsym(RTLD_NEXT, "pthread_mutex_lock");
    }
    int ret = original_pthread_mutex_lock(mutex);
    msh_exit_blockable_call();

    return ret;
}


int (*original_pthread_cond_wait)(pthread_cond_t  *cond,
                                  pthread_mutex_t *mutex) = NULL;

int
pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    msh_enter_blockable_call();
    if (!original_pthread_cond_wait) {
        original_pthread_cond_wait = dlsym(RTLD_NEXT, "pthread_cond_wait");
    }
    int ret = original_pthread_cond_wait(cond, mutex);
    msh_exit_blockable_call();

    return ret;
}

int (*original_pthread_cond_timedwait)(
    pthread_cond_t *restrict cond, pthread_mutex_t *restrict mutex,
    const struct timespec *restrict abstime) = NULL;

int
pthread_cond_timedwait(pthread_cond_t *restrict cond,
                       pthread_mutex_t *restrict mutex,
                       const struct timespec *restrict abstime) {
    msh_enter_blockable_call();
    if (!original_pthread_cond_timedwait)
        original_pthread_cond_timedwait =
            dlsym(RTLD_NEXT, "pthread_cond_timedwait");
    int ret = original_pthread_cond_timedwait(cond, mutex, abstime);
    msh_exit_blockable_call();

    return ret;
}

int (*original_pthread_barrier_wait)(pthread_barrier_t *barrier) = NULL;

int
pthread_barrier_wait(pthread_barrier_t *barrier) {
    msh_enter_blockable_call();
    if (!original_pthread_barrier_wait)
        original_pthread_barrier_wait =
            dlsym(RTLD_NEXT, "pthread_barrier_wait");
    int ret = original_pthread_barrier_wait(barrier);
    msh_exit_blockable_call();

    return ret;
}

int (*original_pthread_join) (pthread_t thread, void **retval) = NULL;

bool first_join = true;
#define CAS(ptr, old_val, new_val)                                             \
    __sync_bool_compare_and_swap(ptr, old_val, new_val)
int
pthread_join(pthread_t thread, void **retval) {
    if (!getenv("SKIP_FIRST_THREAD")) {
        if (CAS(&first_join, true, false)) {
            int err = msh_cleanup();
            if (err) {
                fprintf(stderr, "msh_cleanup() failed\n");
                return -1;
            }
        }
    }
    if (!original_pthread_join)
        original_pthread_join = dlsym(RTLD_NEXT, "pthread_join");
    int ret = original_pthread_join(thread, retval);
    return ret;
}

/**
 * pthread non-blockable calls -- we need to intercept these calls to match the version
*/
int (*original_pthread_cond_signal)(pthread_cond_t *cond) = NULL;

int
pthread_cond_signal(pthread_cond_t *cond) {
    if (!original_pthread_cond_signal) {
        original_pthread_cond_signal = dlsym(RTLD_NEXT, "pthread_cond_signal");
    }
    int ret = original_pthread_cond_signal(cond);

    return ret;
}

int (*original_pthread_mutex_unlock)(pthread_mutex_t *mutex) = NULL;

int
pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (!original_pthread_mutex_unlock) {
        original_pthread_mutex_unlock =
            dlsym(RTLD_NEXT, "pthread_mutex_unlock");
    }
    int ret = original_pthread_mutex_unlock(mutex);

    return ret;
}
