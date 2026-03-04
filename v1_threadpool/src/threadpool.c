#include "threadpool.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>


typedef struct task_node {
    client_task_t task;
    struct task_node *next;
} task_node_t;

struct thread_pool {
    pthread_t *threads;
    int thread_count;
    task_node_t *head;
    task_node_t *tail;

    pthread_mutex_t mtx;
    pthread_cond_t cond;
    int stop;
    void (*handler)(client_task_t *);
};

static void *worker_routine(void *arg) {
    thread_pool_t *pool = (thread_pool_t *)arg;

    for(;;) {
        pthread_mutex_lock(&pool->mtx);

        while(!pool->stop && pool->head == NULL) {
            pthread_cond_wait(&pool->cond, &pool->mtx);
        }

        if(pool->stop && pool->head == NULL) {
            pthread_mutex_unlock(&pool->mtx);
            break;
        }

        task_node_t *node = pool->head;
        pool->head = node->next;
        if(pool->head == NULL) {
            pool->tail = NULL;
        }
        pthread_mutex_unlock(&pool->mtx);

        pool->handler(&node->task);
        free(node);
    }

    return NULL;
}


thread_pool_t *thread_pool_create(int nthreads, void (*handler)(client_task_t *)) {
    if(nthreads <= 0 || handler == NULL) {
        return NULL;
    }

    thread_pool_t *pool = (thread_pool_t *)calloc(1, sizeof(thread_pool_t));
    if(!pool) {
        return NULL;
    }

    pool->threads = (pthread_t *)calloc((size_t)nthreads, sizeof(pthread_t));
    if(!pool->threads) {
        free(pool);
        return NULL;
    }

    pool->thread_count = nthreads;
    pool->handler = handler;
    pool->head = pool->tail = NULL;
    pool->stop = 0;

    if(pthread_mutex_init(&pool->mtx, NULL) != 0) {
        free(pool->threads);
        free(pool);
        return NULL;
    }

    if(pthread_cond_init(&pool->cond, NULL) != 0) {
        pthread_mutex_destroy(&pool->mtx);
        free(pool->threads);
        free(pool);
        return NULL;
    }

    for(int i = 0; i < nthreads; ++i) {
        if(pthread_create(&pool->threads[i], NULL, worker_routine, pool) != 0) {
            pool->stop = 1;
            pthread_cond_broadcast(&pool->cond);
            for(int j = 0; j < i; ++j) {
                pthread_join(pool->threads[j], NULL);
            }
            pthread_cond_destroy(&pool->cond);
            pthread_mutex_destroy(&pool->mtx);
            free(pool->threads);
            free(pool);
            return NULL;
        }
    }

    return pool;
}

int thread_pool_submit(thread_pool_t *pool, const client_task_t *task) {
    if(!pool || !task) {
        return -1;
    }

    task_node_t *node = (task_node_t *)malloc(sizeof(task_node_t));
    if(!node) {
        return -1;
    }

    node->task = *task;
    node->next = NULL;

    pthread_mutex_lock(&pool->mtx);

    if(pool->stop) {
        pthread_mutex_unlock(&pool->mtx);
        free(node);
        return -1;
    }

    if(pool->tail) {
        pool->tail->next = node;
    } else {
        pool->head = node;
    }
    pool->tail = node;

    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mtx);

    return 0;
}

void thread_pool_destroy(thread_pool_t *pool) {
    if(!pool) {
        return;
    }

    pthread_mutex_lock(&pool->mtx);
    pool->stop = 1;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->mtx);

    for(int i = 0; i < pool->thread_count; ++i) {
        pthread_join(pool->threads[i], NULL);
    }

    task_node_t *cur = pool->head;
    while(cur) {
        task_node_t *next = cur->next;
        free(cur);
        cur = next;
    }

    pthread_cond_destroy(&pool->cond);
    pthread_mutex_destroy(&pool->mtx);
    free(pool->threads);
    free(pool);

    return;

}
