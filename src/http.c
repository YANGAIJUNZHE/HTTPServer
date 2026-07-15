#include"http.h"
#include"buffer.h"
#include<stdio.h>
#include<string.h>


//在buf中找needle子串，返回头指针
static const char *find_substr(const char* buf,size_t len,const char *needle){
    size_t nlen=strlen(needle);
    if(nlen>len){
        return NULL;
    }
    for(size_t i=0;i+nlen<=len;++i){
        if(memcmp(buf+i,needle,nlen)==0){
            return buf+i;
        }
    }
    return NULL;
}

//一，解析请求


int http_parse(const char* buf,size_t len,const char **method,size_t *mlen,const char**uri,size_t *ulen){
    const char*sp1,*sp2,*end;
    end=find_substr(buf,len,"\r\n");
    if(end==NULL){
        return HTTP_PARSE_BAD;
    }

    sp1=find_substr(buf,(size_t)(end-buf)," ");
    if(sp1==NULL){
        return HTTP_PARSE_BAD;
    }

    sp2=find_substr(sp1+1,(size_t)(end-sp1-1)," ");
    if(sp2==NULL){
        return HTTP_PARSE_BAD;
    }

    *method=buf;
    *mlen=(size_t)(sp1-buf);

    *uri=sp1+1;
    *ulen=(size_t)(sp2-sp1-1);

    if(*mlen!=3||memcmp(*method,"GET",3)!=0){
        return HTTP_PARSE_NOIMPL;//501
    }

    if(*ulen==0){
        return HTTP_PARSE_BAD;//空，400
    }
    if(**uri!='/'){
        return HTTP_PARSE_BAD;//不以/开头，400
    }
    if(find_substr(*uri,*ulen,"..")){
        return HTTP_PARSE_BAD;//先简单检测一下路径穿越
    }

    return HTTP_PARSE_OK;//成功

}

//二，构造响应

const char*http_reason(int status){
    switch(status){
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 501: return "Not Implemented";
        default: return "Unknown";
    }
} 

int http_build_200(char *buf, size_t cap, const char *mime, off_t size)
  {
      return snprintf(buf, cap,
          "HTTP/1.1 200 OK\r\n"
          "Content-Type: %s\r\n"
          "Content-Length: %ld\r\n"
          "Connection: keep-alive\r\n"
          "Server: clover/1.0\r\n"
          "\r\n",
          mime, size);
  }
static const char *error_body =
      "<!DOCTYPE html>\r\n"
      "<html><head><title>%d %s</title></head>\r\n"
      "<body><h1>%d %s</h1></body>\r\n"
      "</html>\r\n";

int http_build_error(char* buf,size_t cap,int status){
    const char*reason=http_reason(status);
    char body[128];
    int body_len=snprintf(body,sizeof(body),error_body,status,reason,status,reason);
    int total=snprintf(buf,cap,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: keep-alive\r\n"
        "Server: clover/1.0\r\n"
        "\r\n"
        "%s",
        status,reason,body_len,body
    );
    return total;
}
