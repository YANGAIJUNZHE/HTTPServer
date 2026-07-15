#include "http.h"
#include "file.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>

char g_root[256];

// URL 解码：将 %XX 转换为对应字节，原地修改，返回解码后长度
static size_t url_decode(char *s, size_t len)
{
    size_t i = 0, j = 0;
    while (i < len) {
        if (s[i] == '%' && i + 2 < len) {
            char hex[3] = {s[i+1], s[i+2], '\0'};
            char *end;
            long val = strtol(hex, &end, 16);
            if (end == hex + 2) {
                s[j++] = (char)val;
                i += 3;
                continue;
            }
        }
        s[j++] = s[i++];
    }
    return j;
}

static int parse_request(const char *buf,char *method,char *path){
    //找请求行结尾 \r\n
    const char *end=strstr(buf,"\r\n");
    if(!end) return 400;
    //解析方法
    int i=0,j=0;
    while(buf[i]!='\0'&&!isspace((unsigned char)buf[i])&&j<15){
        method[j++]=buf[i++];
    }
    method[j]='\0';
    //检查方法
    if(strcasecmp(method,"GET"))
        return 501;
    //跳过空白
    while(buf[i]!='\0'&&isspace((unsigned char)buf[i])) i++;
    if(buf[i]=='\0') return 400;
    //解析path
    j=0;
    while(buf[i]!='\0'&&!isspace((unsigned char)buf[i])&&j<255){
        path[j++]=buf[i++];
    }
    path[j]='\0';
    j=(int)url_decode(path,j);
    path[j]='\0';
    if(strstr(path,"..")) return 400;
    //默认首页
    if(strcmp(path,"/")==0)
        strcpy(path,"/index.html");
    return 200;
}

static int build_error(int code, char *hdr, size_t hdr_size) {
    const char *msg;
    if (code == 400) msg = "Bad Request";
    else if (code == 404) msg = "Not Found";
    else msg = "Not Implemented";
    return snprintf(hdr, hdr_size,
        "HTTP/1.1 %d %s\r\n"
        "Server: jdbhttpd/0.1.0\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<HTML><TITLE>%d %s</TITLE>\r\n"
        "<BODY><P>%d %s</P></BODY></HTML>\r\n",
        code, msg, code, msg, code, msg);
}


int accept_request(char *rbuf,int *rlen,struct response *resp){
    //找头部结束 \r\n\r\n
    char *end=strstr(rbuf,"\r\n\r\n");
    if(!end) return 0; //数据不完整，需要更多
    int consumed=(end-rbuf)+4;
    //解析请求行
    char method[16],path[256];
    int ret=parse_request(rbuf,method,path);
    if(ret==200){
        ret=prepare_response(path,g_root,resp);
    }
    if(ret!=200){
        resp->file_fd=-1;
        resp->file_size=0;
        resp->hdr_len = build_error(ret, resp->hdr, sizeof(resp->hdr));
    }
    //排空已消费的头部，剩余数据前移
    memmove(rbuf,rbuf+consumed,*rlen-consumed);
    *rlen-=consumed;
    return 1;
}
