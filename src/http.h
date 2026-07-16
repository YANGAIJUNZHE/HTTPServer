#ifndef HTTP_H
#define HTTP_H
#include <sys/types.h>

struct response {
    char  hdr[4096];
    int   hdr_len;
    int   file_fd;
    off_t file_size;
    int   keep_alive;    // 1 = keep-alive, 0 = close
    int   use_thread;    // 1 = 用线程池发文件, 0 = 主线程 sendfile
};

extern char g_root[256];

// 初始化 response 为安全默认值（file_fd=-1, keep_alive=0, use_thread=0）
void response_init(struct response *resp);
// 设置 HTML 响应 —— 一次性填充所有字段
void response_html(struct response *resp, int code, const char *title, const char *body);
// 设置重定向响应
void response_redirect(struct response *resp, int code, const char *location, const char *set_cookie);
// 设置简单错误响应（纯文本 HTML）
void response_error(struct response *resp, int code, const char *msg);

// 设置 session 超时时间（秒），应在 server 启动前调用
void http_set_session_timeout(int sec);

// 解析 HTTP 请求，填充 response
// 返回 1=解析完成, 0=数据不完整
int accept_request(char *rbuf, int *rlen, struct response *resp);

#endif