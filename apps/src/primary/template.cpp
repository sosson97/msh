#include <pthread.h>

#define THREAD_NUM 1

void *do_nothing(void *arg) {
    return NULL;
}

int main() {
    pthread_t t[THREAD_NUM];
    for (int i = 0; i < THREAD_NUM; i++)
        pthread_create(&t[i], NULL, do_nothing, NULL);

    for (int i = 0; i < THREAD_NUM; i++)
        pthread_join(t[i], NULL);
    return 0;
}
