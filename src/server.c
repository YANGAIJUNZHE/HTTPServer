#include "server.h"
#include "http.h"
#include "file.h"
#include "buffer.h"

#include <stdio.h>        // perror, fprintf
#include <stdlib.h>       // calloc, free, realpath
#include <string.h>       // strlen, memset
#include <unistd.h>       // close, read, write
#include <fcntl.h>        // fcntl, O_NONBLOCK
#include <errno.h>        // errno
#include <signal.h>       // signal, SIGPIPE, SIG_IGN
#include <sys/socket.h>   // socket, bind, listen, accept, setsockopt
#include <sys/epoll.h>    // epoll_create1, epoll_ctl, epoll_wait
#include <sys/sendfile.h> // sendfile
#include <netinet/in.h>   // sockaddr_in, htons, INADDR_ANY
#include <limits.h>       // PATH_MAX

// 常量
#define MAX_CONNS 512
#define MAX_EVENTS 1024
#define BACKLOG 128

// 连接槽位

typedef struct
{
    int fd; // -1 表示空闲
    char req_buf[REQ_BUF_SIZE];
    size_t req_len;
    char resp_hdr[HDR_BUF_SIZE];
    size_t resp_hdr_len;
    size_t resp_hdr_sent;
    int file_fd; // -1 表示无文件
    off_t file_size;
    off_t file_sent; // sendfile 用 off_t
} conn_t;

struct server_t
{
    int listen_fd;
    int epoll_fd;
    char root_real[PATH_MAX]; // 启动时 realpath(root_dir) 一次
    size_t root_len;
    uint16_t port;
    conn_t *connections; // 固定数组连接池
    int max_conns;
    volatile sig_atomic_t running; // 信号安全退出标志
};

// 辅助函数
static int set_nonblock(int fd);
static void handle_accept(server_t *s);
static void handle_read(server_t *s, conn_t *c);
static void handle_write(server_t *s, conn_t *c);
static void close_conn(server_t *s, conn_t *c);
static conn_t *conn_alloc(server_t *s);
static int request_complete(const char *buf, size_t len);

// set_nonblock — fcntl 设置 O_NONBLOCK

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// request_complete — 检测 HTTP 请求头是否接收完
// 当前实现：找 \r\n\r\n（GET 请求没有 body）,后续可以扩展

static int request_complete(const char *buf, size_t len)
{
    for (size_t i = 0; i < len - 3; ++i)
    {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
        {
            return 1;
        }
    }
    return 0;
}

// conn_alloc — 从连接池找空闲槽位

static conn_t *conn_alloc(server_t *s)
{
    for (int i = 0; i < s->max_conns; i++)
    {
        if (s->connections[i].fd == -1)
        {
            return &s->connections[i];
        }
    }
    return NULL; // 池满，拒绝新连接
}

// close_conn 统一资源清理

static void close_conn(server_t *s, conn_t *c)
{
    if (c->fd >= 0)
    {
        epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
        close(c->fd);
        c->fd = -1;
    }
    if (c->file_fd >= 0)
    {
        close(c->file_fd);
        c->file_fd = -1;
    }
    // 重置其他字段
    c->req_len = 0;
    c->resp_hdr_len = 0;
    c->resp_hdr_sent = 0;
    c->file_size = 0;
    c->file_sent = 0;
}

// handle_accept — 接受新连接

static void handle_accept(server_t *s)
{
    while (1)
    {
        int fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break; // 没积压连接了
            }
            perror("[ERROR] accept");
            break;
        }

        // 设非阻塞
        if (set_nonblock(fd) == -1)
        {
            perror("[ERROR] set_nonblock client");
            close(fd);
            continue;
        }

        // 取连接池槽位
        conn_t *c = conn_alloc(s);
        if (c == NULL)
        {
            fprintf(stderr, "[WARN] 连接池满 (%d)，拒绝连接\n",
                    s->max_conns);
            close(fd);
            continue;
        }

        // 初始化槽位
        memset(c, 0, sizeof(*c));
        c->fd = fd;
        c->file_fd = -1;

        // 注册到 epoll：LT 模式，先只监听读
        struct epoll_event ev;
        ev.events = EPOLLIN; // LT，不加 EPOLLET
        ev.data.ptr = c;     // 事件到达时直接取 conn_t*
        if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1)
        {
            perror("[ERROR] epoll_ctl ADD client");
            close(fd);
            c->fd = -1;
        }
    }
}

// handle_read — 读请求 → 解析 → 文件服务 → 构建响应 → 切写模式

static void handle_read(server_t *s, conn_t *c)
{
    // 读数据
    size_t remain = REQ_BUF_SIZE - c->req_len;
    ssize_t n = read(c->fd, c->req_buf + c->req_len, remain);

    if (n > 0)
    {
        c->req_len += (size_t)n;
    }
    else if (n == 0)
    {
        // 客户端正常关闭 TCP 连接
        close_conn(s, c);
        return;
    }
    else
    {
        // n == -1
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return; // LT 会再次通知
        }
        close_conn(s, c); // 读错误
        return;
    }

    // 请求头接收完整
    if (!request_complete(c->req_buf, c->req_len))
    {
        /* 没完 + 超 8KB → 请求头太大，400 拒绝 */
        if (c->req_len >= REQ_BUF_SIZE)
        {
            c->resp_hdr_len = http_build_error(c->resp_hdr,
                                               HDR_BUF_SIZE, 400);
            c->resp_hdr_sent = 0;
            c->file_fd = -1;
            c->file_size = 0;
            c->file_sent = 0;
            goto mod_write;
        }
        return; // 等更多数据
    }

    // HTTP 解析
    const char *method, *uri;
    size_t mlen, ulen;
    int pr = http_parse(c->req_buf, c->req_len,
                        &method, &mlen, &uri, &ulen);

    if (pr == HTTP_PARSE_NOIMPL)
    {
        c->resp_hdr_len = http_build_error(c->resp_hdr,
                                           HDR_BUF_SIZE, 501);
        goto skip_file;
    }
    if (pr == HTTP_PARSE_BAD)
    {
        c->resp_hdr_len = http_build_error(c->resp_hdr,
                                           HDR_BUF_SIZE, 400);
        goto skip_file;
    }

    // 文件服务
    int file_fd, status;
    off_t file_size;
    const char *mime;

    int fr = file_serve(uri, ulen,
                        s->root_real, s->root_len,
                        &file_fd, &file_size, &mime, &status);
    if (fr == -1)
    {
        c->resp_hdr_len = http_build_error(c->resp_hdr,
                                           HDR_BUF_SIZE, status);
        goto skip_file;
    }

    // 构造 200 响应头
    c->resp_hdr_len = http_build_200(c->resp_hdr, HDR_BUF_SIZE,
                                     mime, file_size);
    c->file_fd = file_fd;
    c->file_size = file_size;
    c->file_sent = 0;
    goto mod_write;

skip_file:
    c->file_fd = -1;
    c->file_size = 0;
    c->file_sent = 0;

mod_write:
    c->resp_hdr_sent = 0;

    // 切 EPOLLOUT
    struct epoll_event ev;
    ev.events = EPOLLOUT;
    ev.data.ptr = c;
    if (epoll_ctl(s->epoll_fd, EPOLL_CTL_MOD, c->fd, &ev) == -1)
    {
        perror("[ERROR] epoll_ctl MOD EPOLLOUT");
        close_conn(s, c);
    }
}

// handle_write — 发送响应（先头部，再文件体）

static void handle_write(server_t *s, conn_t *c)
{
    // 发送响应头
    if (c->resp_hdr_sent < c->resp_hdr_len)
    {
        size_t remain = c->resp_hdr_len - c->resp_hdr_sent;
        ssize_t n = write(c->fd,
                          c->resp_hdr + c->resp_hdr_sent,
                          remain);
        if (n > 0)
        {
            c->resp_hdr_sent += (size_t)n;
        }
        else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return; // LT 下次再通知
        }
        else
        {
            close_conn(s, c);
            return;
        }
    }

    // sendfile 文件体
    if (c->resp_hdr_sent >= c->resp_hdr_len && c->file_fd >= 0)
    {
        while ((off_t)c->file_sent < c->file_size)
        {
            off_t to_send = c->file_size - (off_t)c->file_sent;
            ssize_t n = sendfile(c->fd, c->file_fd,
                                 &c->file_sent, to_send);
            if (n > 0)
            {
                // sendfile 自动推进 c->file_sent，继续循环
                continue;
            }
            else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                return; // LT 下次再通知
            }
            else
            {
                close_conn(s, c);
                return;
            }
        }
    }

    // keep-alive
    if (c->file_fd >= 0)
    {
        close(c->file_fd);
        c->file_fd = -1;
    }
    c->req_len = 0;
    c->resp_hdr_len = 0;
    c->resp_hdr_sent = 0;
    c->file_size = 0;
    c->file_sent = 0;

    // 切回 EPOLLIN，等待下一次 HTTP 请求
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = c;
    if (epoll_ctl(s->epoll_fd, EPOLL_CTL_MOD, c->fd, &ev) == -1)
    {
        perror("[ERROR] epoll_ctl MOD EPOLLIN (keep-alive)");
        close_conn(s, c);
    }
}

// server_create — 启动初始化

server_t *server_create(const config_t *cfg)
{
    server_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->listen_fd = -1;
    s->epoll_fd = -1;
    s->running = 0;

    // realpath(root_dir)
    if (realpath(cfg->root_dir, s->root_real) == NULL)
    {
        fprintf(stderr, "[ERROR] root_dir 无效: %s\n", cfg->root_dir);
        goto fail;
    }
    s->root_len = strlen(s->root_real);
    s->port = cfg->port;

    // socket
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0)
    {
        perror("[ERROR] socket");
        goto fail;
    }

    // SO_REUSEADDR端口释放
    int opt = 1;
    if (setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
    {
        perror("[ERROR] setsockopt SO_REUSEADDR");
        goto fail;
    }

    // bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(s->port);
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("[ERROR] bind");
        goto fail;
    }

    // listen
    if (listen(s->listen_fd, BACKLOG) < 0)
    {
        perror("[ERROR] listen");
        goto fail;
    }

    // 监听 fd 非阻塞
    if (set_nonblock(s->listen_fd) == -1)
    {
        perror("[ERROR] set_nonblock listen_fd");
        goto fail;
    }

    // epoll_create
    s->epoll_fd = epoll_create1(0);
    if (s->epoll_fd < 0)
    {
        perror("[ERROR] epoll_create1");
        goto fail;
    }

    // epoll 注册监听 fd（data.ptr=NULL 标记）
    struct epoll_event ev;
    ev.events = EPOLLIN; // LT 模式
    ev.data.ptr = NULL;  // NULL = 监听 fd
    if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, s->listen_fd, &ev) == -1)
    {
        perror("[ERROR] epoll_ctl ADD listen_fd");
        goto fail;
    }

    // 连接池：calloc 一次，运行期零 malloc
    s->max_conns = MAX_CONNS;
    s->connections = calloc((size_t)s->max_conns, sizeof(conn_t));
    if (!s->connections)
    {
        perror("[ERROR] calloc connections");
        goto fail;
    }
    for (int i = 0; i < s->max_conns; i++)
    {
        s->connections[i].fd = -1;
        s->connections[i].file_fd = -1;
    }

    // SIGPIPE
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "[INFO] 服务器初始化: root=%s port=%d\n",
            s->root_real, s->port);
    return s;

fail:
    if (s->listen_fd >= 0)
        close(s->listen_fd);
    if (s->epoll_fd >= 0)
        close(s->epoll_fd);
    free(s);
    return NULL;
}

// server_run — 主事件循环（阻塞，直到 server_stop 被调用）

void server_run(server_t *s)
{
    struct epoll_event events[MAX_EVENTS];

    s->running = 1;
    fprintf(stderr, "[INFO] 服务器启动 http://localhost:%d\n", s->port);

    while (s->running)
    {
        int nfds = epoll_wait(s->epoll_fd, events, MAX_EVENTS, 1000);

        if (nfds == -1)
        {
            if (errno == EINTR)
                continue; // 被信号打断
            perror("[ERROR] epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            uint32_t ev = events[i].events;
            conn_t *c = events[i].data.ptr;

            if (c == NULL)
            {
                // 是监听fd
                if (ev & EPOLLIN)
                {
                    handle_accept(s);
                }
            }
            else
            {
                // 是业务 fd

                // 错误 / 挂起优先
                if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
                {
                    close_conn(s, c);
                    continue;
                }

                // 读写可能同时就绪，所以用if
                if (ev & EPOLLIN)
                {
                    handle_read(s, c);
                }
                if (ev & EPOLLOUT)
                {
                    handle_write(s, c);
                }
            }
        }
    }
}

// server_stop — 通知事件循环退出（供信号处理器调用）

void server_stop(server_t *s)
{
    s->running = 0;
}

// 释放全部资源

void server_destroy(server_t *s)
{
    if (!s)
        return;

    // 关闭所有活跃连接
    for (int i = 0; i < s->max_conns; i++)
    {
        if (s->connections[i].fd >= 0)
        {
            close_conn(s, &s->connections[i]);
        }
    }
    free(s->connections);

    if (s->epoll_fd >= 0)
        close(s->epoll_fd);
    if (s->listen_fd >= 0)
        close(s->listen_fd);

    fprintf(stderr, "[INFO] 服务器已关闭\n");
    free(s);
}
