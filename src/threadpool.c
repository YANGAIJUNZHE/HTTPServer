#include "threadpool.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <errno.h>

#define CHUNK_SIZE  (256UL * 1024)  // 每块 256KB，平衡性能和交互

// 任务队列节点
typedef struct task_node {
    file_task_t         task;
    struct task_node   *next;
} task_node_t;

struct threadpool {
    pthread_t      *workers;
    int             nthreads;
    int             running;

    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    task_node_t    *head;
    task_node_t    *tail;
    int             pending;    // 等待数
};

// --- 工作线程 ---
static void *worker_loop(void *arg) {
    threadpool_t *tp = (threadpool_t *)arg;

    while (1) {
        pthread_mutex_lock(&tp->mutex);
        while (tp->running && tp->pending == 0) {
            pthread_cond_wait(&tp->cond, &tp->mutex);
        }
        if (!tp->running) {
            pthread_mutex_unlock(&tp->mutex);
            break;
        }
        // 取一个任务，先拷贝到栈上再释放节点
        task_node_t *node = tp->head;
        tp->head = node->next;
        if (!tp->head) tp->tail = NULL;
        tp->pending--;
        pthread_mutex_unlock(&tp->mutex);

        file_task_t t_copy = node->task;  // 拷贝到栈上
        free(node);

        file_task_t *t = &t_copy;

        // 临时设 socket 为阻塞模式，让 sendfile 高效传输
        int old_flags = fcntl(t->conn_fd, F_GETFL, 0);
        fcntl(t->conn_fd, F_SETFL, old_flags & ~O_NONBLOCK);

        // 分块 sendfile，直到发完或被取消
        off_t off = t->offset;
        size_t rem = t->remain;
        while (rem > 0 && !*(t->cancelled_ptr)) {
            size_t chunk = rem < CHUNK_SIZE ? rem : CHUNK_SIZE;
            ssize_t n = sendfile(t->conn_fd, t->file_fd, &off, chunk);
            if (n <= 0) {
                fprintf(stderr, "[TPOOL] sendfile error: n=%zd errno=%d\n", n, errno);
                break;
            }
            rem -= (size_t)n;
        }
        t->offset = off;
        t->remain = rem;

        // 恢复非阻塞
        fcntl(t->conn_fd, F_SETFL, old_flags);

        // 通过 eventfd 通知主线程（写 1）
        uint64_t val = 1;
        write(t->notify_fd, &val, sizeof(val));
    }
    return NULL;
}

// --- 创建 ---
threadpool_t *tpool_create(int threads) {
    if (threads <= 0) {
        threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (threads < 1) threads = 1;
    }

    threadpool_t *tp = calloc(1, sizeof(*tp));
    tp->nthreads  = threads;
    tp->running   = 1;

    pthread_mutex_init(&tp->mutex, NULL);
    pthread_cond_init(&tp->cond, NULL);

    tp->workers = malloc(sizeof(pthread_t) * (size_t)threads);
    for (int i = 0; i < threads; i++) {
        pthread_create(&tp->workers[i], NULL, worker_loop, tp);
    }
    printf("[INFO] threadpool created with %d threads\n", threads);
    return tp;
}

// --- 提交任务 ---
int tpool_submit(threadpool_t *tp, file_task_t *task) {
    if (!tp->running) return -1;

    task_node_t *node = malloc(sizeof(*node));
    if (!node) return -1;
    node->task = *task;
    node->next = NULL;

    pthread_mutex_lock(&tp->mutex);
    if (tp->tail) {
        tp->tail->next = node;
    } else {
        tp->head = node;
    }
    tp->tail = node;
    tp->pending++;
    pthread_cond_signal(&tp->cond);
    pthread_mutex_unlock(&tp->mutex);
    return 0;
}

// --- 主线程取完成通知 ---
void tpool_complete(threadpool_t *tp) {
    (void)tp;  // 主线程通过 epoll 收 eventfd 通知，不在这里处理
    // 保留作为未来扩展点
}

// --- 销毁 ---
void tpool_destroy(threadpool_t *tp) {
    pthread_mutex_lock(&tp->mutex);
    tp->running = 0;
    pthread_cond_broadcast(&tp->cond);
    pthread_mutex_unlock(&tp->mutex);

    for (int i = 0; i < tp->nthreads; i++) {
        pthread_join(tp->workers[i], NULL);
    }

    // 清理残留任务
    task_node_t *n = tp->head;
    while (n) {
        task_node_t *next = n->next;
        free(n);
        n = next;
    }

    pthread_mutex_destroy(&tp->mutex);
    pthread_cond_destroy(&tp->cond);
    free(tp->workers);
    free(tp);
    printf("[INFO] threadpool destroyed\n");
}

int tpool_size(threadpool_t *tp) {
    return tp->nthreads;
}