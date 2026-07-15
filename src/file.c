#include "file.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static const char *get_mime(const char *path){
    const char *ext=strrchr(path,'.');
    if(!ext) return "application/octet-stream";
    if(strcasecmp(ext,".html")==0||strcasecmp(ext,".htm")==0) return "text/html";
    if(strcasecmp(ext,".css")==0) return "text/css";
    if(strcasecmp(ext,".js")==0) return "application/javascript";

    if(strcasecmp(ext,".png")==0) return "image/png";
    if(strcasecmp(ext,".zip")==0) return "application/zip";
    return "application/octet-stream";
}

int prepare_response(const char *path,const char *root,struct response *resp){
    //拼接完整路径
    char full[512];
    strcpy(full,root);
    strcat(full,path);
    //安全校验：防目录遍历
    char rp[512],rr[512];
    if(!realpath(full,rp)||!realpath(root,rr))
        return 404;
    if(strncmp(rp,rr,strlen(rr))!=0)
        return 404;
    //打开文件
    int fd=open(rp,O_RDONLY);
    if(fd<0) return 404;
    //获取文件信息
    struct stat st;
    if(fstat(fd,&st)<0){
        close(fd);
        return 404;
    }
    //构造响应头
    const char *mime=get_mime(rp);
    resp->hdr_len=sprintf(resp->hdr,
        "HTTP/1.1 200 OK\r\n"
        "Server: jdbhttpd/0.1.0\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "\r\n",mime,st.st_size);
    resp->file_fd=fd;
    resp->file_size=st.st_size;
    return 200;
}
