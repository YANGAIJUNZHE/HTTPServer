#include "server.h"
#include "config.h"
#include "http.h"
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

#define MAX_EVENTS  1024
#define MAX_CONNS   1024

// 时间轮参数
#define TIMER_SLOT         512   // 槽位数
#define TIMER_GRANULARITY  100   // 每格 100ms，512*100ms = 51.2s 覆盖范围
#define KEEP_ALIVE_TIMEOUT 5000  // keep-alive 空闲超时(ms)

struct conn {
    int   fd;
    char  rbuf[4096];
    int   rlen;
    struct response resp;
    int   hdr_sent;
    off_t file_off;
    size_t file_left;

    wtimer_t *timer;               // 指向该连接当前挂载的定时器节点
};

// 全局状态
static struct sockaddr_in serv_addr, cli_addr;
static struct epoll_event ev, events[MAX_EVENTS];
static int listen_fd, epfd;
static socklen_t cliaddr_len;
static int running;
static int nconn, max_conn;
static struct conn conns[MAX_CONNS];

// 时间轮
static wtimer_t *slots[TIMER_SLOT];
static int      current_slot;       // 指针当前位置
static uint64_t last_tick_ms;       // 上次 tick 的时间戳

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

    // 插入 slots[t->slot] 链表头
    t->prev = NULL;
    t->next = slots[t->slot];
    if (slots[t->slot]) slots[t->slot]->prev = t;
    slots[t->slot] = t;

    c->timer = t;
}

void timer_cancel(struct conn *c) {
    wtimer_t *t = c->timer;
    if (!t) return;

    // 从双向链表摘除
    if (t->prev) t->prev->next = t->next;
    else         slots[t->slot] = t->next;

    if (t->next) t->next->prev = t->prev;

    c->timer = NULL;
    free(t);
}

void timer_tick(void) {
    uint64_t now = timer_now_ms();
    // 按 100ms 粒度推进指针
    while (last_tick_ms + TIMER_GRANULARITY <= now) {
        last_tick_ms += TIMER_GRANULARITY;
        current_slot  = (current_slot + 1) % TIMER_SLOT;

        wtimer_t *t = slots[current_slot];
        while (t) {
            wtimer_t *next = t->next;
            if (t->expire <= now) {
                struct conn *c = t->conn;
                // 从链表摘除
                if (t->prev) t->prev->next = t->next;
                else         slots[current_slot] = t->next;
                if (t->next) t->next->prev = t->prev;

                c->timer = NULL;
                free(t);
                // 关连接
                close(c->resp.file_fd);
                epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                close(c->fd);
                memset(c, 0, sizeof(struct conn));
                nconn--;
            }
            t = next;
        }
    }
}

// 连接关闭
static void conn_close(struct conn *c) {
    timer_cancel(c);
    close(c->resp.file_fd);
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    memset(c, 0, sizeof(struct conn));
    nconn--;
}

// Server 生命周期
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
        perror("setsockopt SO_REUSEADDR"); close(listen_fd); return 0;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port        = htons(port);
    if (bind(listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind address failed"); close(listen_fd); return 0;
    }
    if (listen(listen_fd, backlog) < 0) {
        perror("listen failed"); close(listen_fd); return 0;
    }
    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL, 0) | O_NONBLOCK);

    if ((epfd = epoll_create1(0)) < 0) {
        perror("epoll create failed"); close(listen_fd); return 0;
    }
    ev.events  = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        perror("epoll_ctl add listenfd fail"); close(epfd); close(listen_fd); return 0;
    }

    timer_init();
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
            // 新连接：循环 accept 直到没连接或上限 16 次
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

                    c->fd = conn_fd;
                    c->resp.file_fd = -1;
                    ev.events  = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
                    ev.data.ptr = c;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &ev) < 0) {
                        close(conn_fd);
                        c->fd = 0;
                        nconn--;
                        continue;
                    }
                }
                continue;
            }

            struct conn *c = (struct conn *)events[i].data.ptr;

            // 异常断开
            if (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                conn_close(c);
                continue;
            }

            // 读事件
            if (events[i].events & EPOLLIN) {
                timer_cancel(c);    // 有新数据到来，重置 keep-alive 计时

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
                if (ret == 0) continue;  // 数据不完整，继续读

                // 解析完成，切写模式
                c->hdr_sent  = 0;
                c->file_off  = 0;
                c->file_left = c->resp.file_size;
                ev.events    = EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
                ev.data.ptr  = c;
                if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev) < 0) {
                    conn_close(c);
                    continue;
                }
                continue;
            }

            // 写事件
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

                // 发文件体
                if (done && c->file_left > 0) {
                    ssize_t n = sendfile(c->fd, c->resp.file_fd,
                                         &c->file_off, c->file_left);
                    if (n < 0 && errno == EAGAIN) {
                        done = 0;
                    } else if (n <= 0) {
                        conn_close(c);
                        continue;
                    } else {
                        c->file_left -= n;
                        if (c->file_left > 0) done = 0;
                    }
                }

                if (!done) continue;

                // keep-alive：发完切回读 + 定时器
                close(c->resp.file_fd);
                c->resp.file_fd = -1;
                c->rlen = 0;
                ev.events   = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
                ev.data.ptr = c;
                if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev) < 0) {
                    conn_close(c);
                    continue;
                }
                timer_add(c, KEEP_ALIVE_TIMEOUT);
            }
        }
        timer_tick();
    }

    // 清理所有活跃连接
    for (int i = 0; i < max_conn; i++) {
        if (conns[i].fd > 0) {
            timer_cancel(&conns[i]);
            close(conns[i].resp.file_fd);
            epoll_ctl(epfd, EPOLL_CTL_DEL, conns[i].fd, NULL);
            close(conns[i].fd);
        }
    }

    close(epfd);
    close(listen_fd);
}