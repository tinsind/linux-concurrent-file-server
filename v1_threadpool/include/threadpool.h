#ifndef __THREADPOOL_H__
#define __THREADPOOL_H__

#include <netinet/in.h>

typedef struct {
    int fd;
    struct sockaddr_in addr;
} client_task_t;

typedef struct thread_pool thread_pool_t;

thread_pool_t *thread_pool_create(int nthreads, void (*handler)(client_task_t *));
int thread_pool_submit(thread_pool_t *pool, const client_task_t *task);
void thread_pool_destroy(thread_pool_t *pool);



#endif // __THREADPOOL_H__
