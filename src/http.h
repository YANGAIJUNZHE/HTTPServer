#ifndef HTTP_H
#define HTTP_H
#include <sys/types.h>
struct response{
    char hdr[4096];
    int hdr_len;
    int file_fd;
    off_t file_size;
};
extern char g_root[256];
int accept_request(char *rbuf,int *rlen,struct response *resp);
#endif
