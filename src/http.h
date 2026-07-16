#ifndef HTTP_H
#define HTTP_H
#include <sys/types.h>

struct response {
    char  hdr[4096];
    int   hdr_len;
    int   file_fd;
    off_t file_size;
    // 连接管理字段（由 server 使用）
    int   keep_alive;    // 1 = keep-alive, 0 = close
    int   use_thread;    // 1 = 用线程池发文件, 0 = 主线程 sendfile
    int   notify_fd;     // eventfd，线程完成时通知主线程
};

extern char g_root[256];

// 解析 HTTP 请求，填充 response
// 返回 1=解析完成, 0=数据不完整, -1=解析出错
int accept_request(char *rbuf, int *rlen, struct response *resp);

#endif