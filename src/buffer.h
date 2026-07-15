#ifndef BUFFER_H
#define BUFFER_H

#include<string.h>

#define REQ_BUF_SIZE 8192//HTTPget的最大数
#define HDR_BUF_SIZE 1024//响应头的最大数

//向buffer中追加字符串s,pos是位置指针，cap是缓冲区容量
static inline int buf_puts(char* buf,size_t* pos,size_t cap,const char* s){
    size_t slen=strlen(s);
    if(*pos+slen>=cap){
        return -1;
    }
    memcpy(buf+*pos,s,slen);
    *pos+=slen;
    return 0;
}

#endif