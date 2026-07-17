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
 #define DETHREADS_NUM  4 

// 任务队列节点
typedef struct task_node {
    file_task_t         task;
    struct task_node   *next;
} task_node_t;

struct threadpool {
    pthread_t      *workers;
    int             nthreads;
    int             running;

    pthread_mutex_t mutex;//互斥锁
    pthread_cond_t  cond;//条件变量
    task_node_t    *head;
    task_node_t    *tail;
    int             pending;    // 任务数
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

        int status = SEND_SUCCESS;  // 默认成功

        // 临时设 socket 为阻塞模式，sendfile 实现更简单
        int old_flags = fcntl(t->conn_fd, F_GETFL, 0);
        fcntl(t->conn_fd, F_SETFL, old_flags & ~O_NONBLOCK);

        // 分块发送，直到发完或被取消
        off_t off = t->offset;
        size_t rem = t->remain;
        char *fallback_buf = NULL;

        while (rem > 0 && !*(t->cancelled_ptr)) {
            size_t chunk = rem < CHUNK_SIZE ? rem : CHUNK_SIZE;
            ssize_t n = sendfile(t->conn_fd, t->file_fd, &off, chunk);

            if (n == -1 && (errno == ESPIPE || errno == EINVAL)) {
                // WSL2 9p 等文件系统不支持 sendfile，回退 lseek+read+write
                if (!fallback_buf) {
                    fallback_buf = malloc(CHUNK_SIZE);
                }
                if (!fallback_buf) { status = SEND_FAILED; break; }
                if (lseek(t->file_fd, off, SEEK_SET) == (off_t)-1) {
                    status = SEND_FAILED; break;
                }
                ssize_t rn = read(t->file_fd, fallback_buf, chunk);
                if (rn <= 0) {
                    status = SEND_FAILED; break;
                }
                ssize_t wn = write(t->conn_fd, fallback_buf, (size_t)rn);
                if (wn <= 0) {
                    status = SEND_FAILED; break;
                }
                off += wn;
                rem -= (size_t)wn;
                continue;
            }

            if (n <= 0) {
                status = SEND_FAILED;
                break;
            }
            rem -= (size_t)n;
        }
        free(fallback_buf);

        if (*(t->cancelled_ptr)) {
            status = SEND_CANCELLED;
        }

        // 恢复非阻塞
        fcntl(t->conn_fd, F_SETFL, old_flags);

        // 通过 eventfd 传递 status（写入 status 值而不是 1）
        uint64_t val = (uint64_t)status;
        write(t->notify_fd, &val, sizeof(val));
    }
    return NULL;
}

// --- 创建 ---
threadpool_t *tpool_create(int threads) {
    if (threads <= 0) {
        threads = DETHREADS_NUM;
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
        pthread_join(tp->workers[i], NULL);//回收线程
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