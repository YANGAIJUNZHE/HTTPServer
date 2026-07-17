#ifndef THREADPOOL_H
#define THREADPOOL_H
#define DETHREADS_NUM  4  

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// 文件发送任务状态（通过 eventfd 传给主线程）
#define SEND_SUCCESS  1   // 发送完成
#define SEND_FAILED   2   // 发送失败（sendfile 出错）
#define SEND_CANCELLED 3  // 被 timer 取消

// 文件发送任务
typedef struct file_task {
    int             conn_fd;       // 客户端 fd（工作线程用）
    int             file_fd;       // 源文件 fd
    off_t           offset;        // 已发送偏移
    size_t          remain;        // 剩余字节
    int             notify_fd;     // eventfd，完成后通知主线程
    volatile int   *cancelled_ptr; // 指向 conn 的取消标记
} file_task_t;

// 线程池
typedef struct threadpool threadpool_t;

// 创建线程池，threads=0 则用 CPU 核数
threadpool_t *tpool_create(int threads);

// 提交文件发送任务，返回 0 成功 -1 失败
int tpool_submit(threadpool_t *tp, file_task_t *task);

// 销毁线程池
void tpool_destroy(threadpool_t *tp);

// 获取线程数
int tpool_size(threadpool_t *tp);

#endif