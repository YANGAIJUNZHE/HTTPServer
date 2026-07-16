#include "server.h"
#include "config.h"
#include "http.h"
#include "threadpool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/time.h>
#include <sys/eventfd.h>

#define MAX_EVENTS   1024
#define MAX_CONNS    1024

// 时间轮参数
#define TIMER_SLOT         512
#define TIMER_GRANULARITY  100
#define KEEP_ALIVE_TIMEOUT 5000

// 连接状态
enum {
    CS_READ,         // 等待/接收请求
    CS_SENDING,      // 主线程发送中（小文件/HTML响应）
    CS_THREADING,    // 工作线程正在发送大文件
    CS_DONE,         // 响应完成，等待 keep-alive 或关闭
};

struct conn {
    int      fd;
    char     rbuf[4096];
    int      rlen;
    struct response resp;
    int      state;

    // 发送状态（主线程模式）
    int      hdr_sent;
    off_t    file_off;
    size_t   file_left;

    // 线程模式
    int          notify_fd;    // eventfd：线程完成通知
    volatile int cancelled;    // 取消标记（工作线程轮询）

    wtimer_t *timer;
};

// 全局状态
static struct sockaddr_in serv_addr, cli_addr;
static struct epoll_event ev, events[MAX_EVENTS];
static int listen_fd = -1, epfd = -1;
static socklen_t cliaddr_len;
static int running;
static int nconn, max_conn;
static struct conn conns[MAX_CONNS];

// 时间轮
static wtimer_t *slots[TIMER_SLOT];
static int      current_slot;
static uint64_t last_tick_ms;

// 线程池
static threadpool_t *g_tpool = NULL;

// --- 时间轮（不变） ---

uint64_t timer_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void timer_init(void) {
    memset(slots, 0, sizeof(slots));
    current_slot  = 0;
    last_tick_ms  = timer_now_ms();
}

void timer_add(struct conn *c, uint64_t timeout_ms) {
    wtimer_t *t = malloc(sizeof(wtimer_t));
    t->conn    = c;
    t->expire  = timer_now_ms() + timeout_ms;
    t->slot    = (current_slot + (timeout_ms / TIMER_GRANULARITY)) % TIMER_SLOT;

    t->prev = NULL;
    t->next = slots[t->slot];
    if (slots[t->slot]) slots[t->slot]->prev = t;
    slots[t->slot] = t;

    c->timer = t;
}

void timer_cancel(struct conn *c) {
    wtimer_t *t = c->timer;
    if (!t) return;

    if (t->prev) t->prev->next = t->next;
    else         slots[t->slot] = t->next;

    if (t->next) t->next->prev = t->prev;

    c->timer = NULL;
    free(t);
}

void timer_tick(void) {
    uint64_t now = timer_now_ms();
    while (last_tick_ms + TIMER_GRANULARITY <= now) {
        last_tick_ms += TIMER_GRANULARITY;
        current_slot  = (current_slot + 1) % TIMER_SLOT;

        wtimer_t *t = slots[current_slot];
        while (t) {
            wtimer_t *next = t->next;
            if (t->expire <= now) {
                struct conn *c = t->conn;
                if (t->prev) t->prev->next = t->next;
                else         slots[current_slot] = t->next;
                if (t->next) t->next->prev = t->prev;

                c->timer = NULL;
                free(t);
                // 清理连接：CS_THREADING 状态下不能直接关闭 file_fd
                if (c->state == CS_THREADING) {
                    c->cancelled = 1;
                    // 等待 eventfd 通知后由主循环清理
                    continue;
                }
                if (c->resp.file_fd >= 0) close(c->resp.file_fd);
                if (c->notify_fd >= 0) close(c->notify_fd);
                epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                close(c->fd);
                memset(c, 0, sizeof(struct conn));
                nconn--;
            }
            t = next;
        }
    }
}

// --- 连接关闭 ---

static void conn_close(struct conn *c) {
    timer_cancel(c);

    // CS_THREADING 状态下，file_fd 由工作线程持有，不能直接 close
    // 设置 cancelled 标记让工作线程自行退出
    if (c->state == CS_THREADING) {
        c->cancelled = 1;
        // 不关闭 fd——工作线程写 eventfd 后主线程会收到通知并做最终清理
        return;
    }

    if (c->resp.file_fd >= 0) close(c->resp.file_fd);
    if (c->notify_fd >= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, c->notify_fd, NULL);
        close(c->notify_fd);
    }
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    memset(c, 0, sizeof(struct conn));
    nconn--;
}

// --- 将连接切换到写模式（发送响应头和可能的文件体） ---

static void conn_start_write(struct conn *c) {
    c->hdr_sent  = 0;
    c->file_off  = 0;
    c->file_left = c->resp.file_size;

    // 大文件 → 提交线程池
    if (c->resp.use_thread && c->resp.file_fd >= 0 && c->resp.file_size > 0) {
        c->notify_fd = eventfd(0, EFD_NONBLOCK);
        c->resp.notify_fd = c->notify_fd;
        c->state = CS_THREADING;

        // 先发响应头（主线程）
        int n = send(c->fd, c->resp.hdr, c->resp.hdr_len, 0);
        if (n <= 0) {
            conn_close(c);
            return;
        }
        c->hdr_sent = n;

        // 如果头一次没发完，先把剩余头发完再投线程
        if (c->hdr_sent < c->resp.hdr_len) {
            c->state = CS_SENDING;
            ev.events   = EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
            ev.data.ptr = c;
            epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
            return;
        }

        c->cancelled = 0;

        // 头已发完，投递文件发送任务到线程池
        file_task_t task;
        task.conn_fd       = c->fd;
        task.file_fd       = c->resp.file_fd;
        task.offset        = 0;
        task.remain        = (size_t)c->resp.file_size;
        task.notify_fd     = c->notify_fd;
        task.cancelled_ptr = &c->cancelled;

        // 注册 eventfd 到 epoll
        ev.events   = EPOLLIN | EPOLLERR | EPOLLHUP;
        ev.data.ptr = c;
        epoll_ctl(epfd, EPOLL_CTL_ADD, c->notify_fd, &ev);

        // 从 epoll 移除客户端 fd（工作线程接管写入）
        epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);

        if (tpool_submit(g_tpool, &task) < 0) {
            // 提交失败，回退主线程模式
            epoll_ctl(epfd, EPOLL_CTL_DEL, c->notify_fd, NULL);
            close(c->notify_fd);
            c->notify_fd = -1;
            c->state = CS_SENDING;
            ev.events   = EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
            ev.data.ptr = c;
            epoll_ctl(epfd, EPOLL_CTL_ADD, c->fd, &ev);
        }
        return;
    }

    // 小文件/无文件 → 主线程直接发
    c->state = CS_SENDING;
    ev.events   = EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    ev.data.ptr = c;
    epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

// --- Server 生命周期 ---

int server_create(void) {
    int port, backlog;
    if (get_cfg_int("PORT", &port) < 0)       { fprintf(stderr,"PORT config failed\n");      return 0; }
    if (get_cfg_int("BACKLOG", &backlog) < 0) { fprintf(stderr,"BACKLOG config failed\n");   return 0; }
    if (get_cfg_int("MAX_CONN", &max_conn) < 0){ fprintf(stderr,"MAX_CONN config failed\n");  return 0; }
    if (max_conn > MAX_CONNS) max_conn = MAX_CONNS;
    if (get_cfg_str("ROOT_DIR", g_root) < 0)  { fprintf(stderr,"ROOT_DIR config failed\n");  return 0; }

    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) { perror("socket create failed"); return 0; }

    int op = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &op, sizeof(op)) == -1) {
        perror("setsockopt SO_REUSEADDR"); return 0;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port        = htons(port);
    if (bind(listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind address failed"); return 0;
    }
    if (listen(listen_fd, backlog) < 0) {
        perror("listen failed"); return 0;
    }
    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL, 0) | O_NONBLOCK);

    if ((epfd = epoll_create1(0)) < 0) {
        perror("epoll create failed"); return 0;
    }
    ev.events  = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        perror("epoll_ctl add listenfd fail"); return 0;
    }

    timer_init();

    // 创建线程池（线程数从配置读，默认 4）
    int tpool_threads = 4;
    get_cfg_int("TPOOL_THREADS", &tpool_threads);
    g_tpool = tpool_create(tpool_threads);

    printf("[INFO] server created\n");
    return 1;
}

void server_stop(void) { running = 0; }

void server_run(void) {
    printf("[INFO] server running\n");
    running = 1;
    while (running) {
        int nready = epoll_wait(epfd, events, MAX_EVENTS, TIMER_GRANULARITY);
        if (nready < 0) {
            if (errno == EINTR) { timer_tick(); continue; }
            perror("epoll_wait error");
            break;
        }
        for (int i = 0; i < nready; i++) {
            // 新连接
            if (events[i].data.fd == listen_fd) {
                int batch = 0;
                while (batch < 16) {
                    cliaddr_len = sizeof(cli_addr);
                    int conn_fd = accept(listen_fd, (struct sockaddr *)&cli_addr, &cliaddr_len);
                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept failed");
                        break;
                    }
                    batch++;
                    if (nconn >= max_conn) { close(conn_fd); continue; }

                    fcntl(conn_fd, F_SETFL, O_NONBLOCK);
                    nconn++;

                    char str[1024];
                    inet_ntop(AF_INET, &cli_addr.sin_addr, str, sizeof(str));
                    printf("received from %s at PORT %d\n", str, ntohs(cli_addr.sin_port));

                    struct conn *c = NULL;
                    for (int k = 0; k < max_conn; k++) {
                        if (conns[k].fd == 0) { c = &conns[k]; break; }
                    }
                    if (!c) { close(conn_fd); nconn--; continue; }

                    memset(c, 0, sizeof(*c));
                    c->fd    = conn_fd;
                    c->state = CS_READ;
                    timer_add(c, KEEP_ALIVE_TIMEOUT);
                    ev.events   = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
                    ev.data.ptr = c;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &ev);
                }
                continue;
            }

            struct conn *c = (struct conn *)events[i].data.ptr;

            // ---- 线程完成通知（eventfd）先于异常检测 ----
            if (c->state == CS_THREADING && c->notify_fd >= 0 &&
                (events[i].events & EPOLLIN)) {
                // 读掉 eventfd 的值
                uint64_t val;
                read(c->notify_fd, &val, sizeof(val));

                // 工作线程完成了，清理
                if (c->resp.file_fd >= 0) {
                    close(c->resp.file_fd);
                    c->resp.file_fd = -1;
                }
                epoll_ctl(epfd, EPOLL_CTL_DEL, c->notify_fd, NULL);
                close(c->notify_fd);
                c->notify_fd = -1;

                // 重新注册客户端 fd 到 epoll，切回读模式
                c->state = CS_READ;
                c->rlen  = 0;
                timer_add(c, KEEP_ALIVE_TIMEOUT);
                ev.events   = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
                ev.data.ptr = c;
                epoll_ctl(epfd, EPOLL_CTL_ADD, c->fd, &ev);
                continue;
            }

            // 异常断开
            if (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                conn_close(c);
                continue;
            }

            // ---- 读事件 ----
            if (events[i].events & EPOLLIN) {
                timer_cancel(c);

                int n = read(c->fd, c->rbuf + c->rlen, sizeof(c->rbuf) - c->rlen - 1);
                if (n < 0) {
                    if (errno == EAGAIN) continue;
                    conn_close(c);
                    continue;
                }
                if (n == 0) { conn_close(c); continue; }

                c->rlen += n;
                c->rbuf[c->rlen] = '\0';

                int ret = accept_request(c->rbuf, &c->rlen, &c->resp);
                if (ret == 0) continue;  // 数据不完整

                // 解析完成，开始写响应
                conn_start_write(c);
                continue;
            }

            // ---- 写事件（主线程发送模式） ----
            if (events[i].events & EPOLLOUT) {
                int done = 1;

                // 发响应头
                if (c->hdr_sent < c->resp.hdr_len) {
                    int n = send(c->fd, c->resp.hdr + c->hdr_sent,
                                 c->resp.hdr_len - c->hdr_sent, 0);
                    if (n <= 0) { conn_close(c); continue; }
                    c->hdr_sent += n;
                    if (c->hdr_sent < c->resp.hdr_len) done = 0;
                }

                // 发文件体（主线程 sendfile，仅小文件走这里）
                if (done && c->resp.file_fd >= 0 && c->file_left > 0) {
                    ssize_t n = sendfile(c->fd, c->resp.file_fd,
                                         &c->file_off, c->file_left);
                    if (n < 0 && errno == EAGAIN) {
                        done = 0;
                    } else if (n <= 0) {
                        conn_close(c);
                        continue;
                    } else {
                        c->file_left -= (size_t)n;
                        if (c->file_left > 0) done = 0;
                    }
                }

                if (!done) continue;

                // 发完：清理
                if (c->resp.file_fd >= 0) {
                    close(c->resp.file_fd);
                    c->resp.file_fd = -1;
                }

                // keep-alive 模式下切回读
                if (c->resp.keep_alive) {
                    c->rlen = 0;
                    timer_add(c, KEEP_ALIVE_TIMEOUT);
                    ev.events   = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
                    ev.data.ptr = c;
                    epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
                } else {
                    conn_close(c);
                }
            }
        }

        timer_tick();
    }

    // 清理线程池
    if (g_tpool) {
        tpool_destroy(g_tpool);
        g_tpool = NULL;
    }
}