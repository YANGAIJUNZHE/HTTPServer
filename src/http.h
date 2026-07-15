#ifndef HTTP_H
#define HTTP_H

#include<sys/types.h>//off_t文件偏移量

#define HTTP_PARSE_OK 200
#define HTTP_PARSE_BAD 400
#define HTTP_PARSE_NOIMPL 501

//解析请求头，返回状态码（0=ok，-1=400，-2=501）
int http_parse(const char* buf,size_t len,const char**method,size_t *mlen,const char **uri,size_t *ulen);



//构造响应头，返回字节数
int http_build_200(char* buf,size_t cap,const char *mime,off_t size);
int http_build_error(char *buf,size_t cap,int status);
const char *http_reason(int status);




#endif