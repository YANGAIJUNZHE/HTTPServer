#include "server.h"
#include "config.h"
#include "http.h"
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

#define MAX_EVENTS 1024
#define MAX_CONNS  1024

struct conn{
    int fd;                     // 客户端socket文件描述符，epoll监听绑定该fd
    char rbuf[4096];            // 读缓冲区：存放从客户端读取到的原始HTTP请求报文
    int rlen;                   // 读缓冲区有效数据长度，标记rbuf中已存入多少字节请求数据
    struct response resp;       // HTTP响应信息结构体：封装本次应答需要的响应头、文件句柄、文件大小
    int hdr_sent;               // 标记HTTP响应头是否发送完成 0=未发送 1=已发送
    off_t file_off;             // 文件读取偏移量，记录当前从文件哪个位置继续读取分片数据下发
    size_t file_left;           // 剩余待发送文件字节总数，初始赋值为resp.file_size，递减至0代表文件发送完毕
};

struct sockaddr_in serv_addr,cli_addr;
struct epoll_event ev,events[MAX_EVENTS];
int listen_fd=-1,epfd=-1;
socklen_t cliaddr_len;
int running;
int nconn,max_conn;
struct conn conns[MAX_CONNS];

static void conn_close(struct conn *c){
    if(c->resp.file_fd>=0) close(c->resp.file_fd);
    epoll_ctl(epfd,EPOLL_CTL_DEL,c->fd,NULL);
    close(c->fd);
    memset(c,0,sizeof(struct conn));
    nconn--;
}

int server_create(){
    //配置导入
    int port,backlog;
    if(get_cfg_int("PORT",&port)<0)    {fprintf(stderr,"PORT config failed\n");return 0;}
    if(get_cfg_int("BACKLOG",&backlog)<0)    {fprintf(stderr,"BACKLOG config failed\n");return 0;}
    if(get_cfg_int("MAX_CONN",&max_conn)<0)    {fprintf(stderr,"MAX_CONN config failed\n");return 0;}
    if(max_conn>MAX_CONNS) max_conn=MAX_CONNS;
    if(get_cfg_str("ROOT_DIR",g_root)<0){fprintf(stderr,"ROOT_DIR config failed\n");return 0;}
    if((listen_fd=socket(AF_INET,SOCK_STREAM,0))<0){perror("socket creat failed");return 0;}
    //SO_REUSEADDR端口释放
    int op=1;
    if(setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&op,sizeof(op))==-1){
        perror("setsockopt SO_REUSEADDR");
        return 0;
    }
    //bind
    memset(&serv_addr,0,sizeof(serv_addr));
    serv_addr.sin_family=AF_INET;
    serv_addr.sin_addr.s_addr=htonl(INADDR_ANY);
    serv_addr.sin_port=htons(port);
    if(bind(listen_fd,(struct sockaddr *)&serv_addr,sizeof(serv_addr))<0){
        perror("bind address failed");
        return 0;
    }
    if(listen(listen_fd,backlog)<0){
        perror("listen failed");
        return 0;
    }
    fcntl(listen_fd,F_SETFL,fcntl(listen_fd,F_GETFL,0)|O_NONBLOCK);
    //epoll注册监听
    if((epfd=epoll_create1(0))<0){
        perror("create connect failed");
        return 0;
    }
    ev.events=EPOLLIN;
    ev.data.fd=listen_fd;
    if(epoll_ctl(epfd,EPOLL_CTL_ADD,listen_fd,&ev)<0){
        perror("epoll_ctl add listenfd fail");
        return 0;
    }
    printf("[INFO]create server successful!\n");
    return 1;
}

void server_stop(){running=0;}

void server_run(){
    printf("[INFO]run server successful!\n");
    running=1;
    while(running){
        int nready=epoll_wait(epfd,events,MAX_EVENTS,-1);
        if(nready<0){
            if(errno==EINTR)    continue;
            perror("epoll_wait error\n");
            break;
        }
        for(int i=0;i<nready;i++){
            //新连接
            if(events[i].data.fd==listen_fd){
                cliaddr_len=sizeof(cli_addr);
                int conn_fd=accept(listen_fd,(struct sockaddr *)&cli_addr,&cliaddr_len);
                if(conn_fd<0){
                    perror("accept failed\n");
                    continue;
                }
                if(nconn>=max_conn){
                    close(conn_fd);
                    continue;
                }
                fcntl(conn_fd,F_SETFL,O_NONBLOCK);
                nconn++;
                char str[1024];
                inet_ntop(AF_INET,&cli_addr.sin_addr,str,sizeof(str));
                printf("received from %s at PORT %d\n",str,ntohs(cli_addr.sin_port));
                //找空闲槽位
                struct conn *c=NULL;
                for(int k=0;k<max_conn;k++){
                    if(conns[k].fd==0){
                        c=&conns[k];
                        break;
                    }
                }
                if(!c){
                    close(conn_fd);
                    nconn--;
                    continue;
                }
                c->fd=conn_fd;
                ev.events=EPOLLIN|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
                ev.data.ptr=c;
                epoll_ctl(epfd,EPOLL_CTL_ADD,conn_fd,&ev);
                continue;
            }
            struct conn *c=(struct conn *)events[i].data.ptr;
            //异常断开
            if(events[i].events&(EPOLLHUP|EPOLLERR|EPOLLRDHUP)){
                conn_close(c);
                continue;
            }
            //读事件——收请求，切写
            if(events[i].events&EPOLLIN){
                //读取数据到连接缓冲区
                int n=read(c->fd,c->rbuf+c->rlen,sizeof(c->rbuf)-c->rlen-1);
                if(n<0){
                    if(errno==EAGAIN) continue;
                    conn_close(c);
                    continue;
                }
                if(n==0){
                    conn_close(c);
                    continue;
                }
                c->rlen+=n;
                c->rbuf[c->rlen]='\0';
                //尝试解析请求
                int ret=accept_request(c->rbuf,&c->rlen,&c->resp);
                if(ret==0) continue;  //数据不完整，继续读
                //请求解析完成，切换到写模式
                c->hdr_sent=0;
                c->file_off=0;
                c->file_left=c->resp.file_size;
                ev.events=EPOLLOUT|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
                ev.data.ptr=c;
                epoll_ctl(epfd,EPOLL_CTL_MOD,c->fd,&ev);
                continue;
            }
            //写事件——发头+发文件
            if(events[i].events&EPOLLOUT){
                int done=1;
                //发响应头
                if(c->hdr_sent<c->resp.hdr_len){
                    int n=send(c->fd,c->resp.hdr+c->hdr_sent,c->resp.hdr_len-c->hdr_sent,0);
                    if(n<=0){
                        conn_close(c);
                        continue;
                    }
                    c->hdr_sent+=n;
                    if(c->hdr_sent<c->resp.hdr_len) done=0;
                }
                //发文件体
                if(done&&c->resp.file_fd>=0&&c->file_left>0){
                    ssize_t n=sendfile(c->fd,c->resp.file_fd,&c->file_off,c->file_left);
                    if(n<0&&errno==EAGAIN){
                        //缓冲区满，等下次EPOLLOUT
                        done=0;
                    }
                    else if(n<=0){
                        conn_close(c);
                        continue;
                    }
                    else{
                        c->file_left-=n;
                        if(c->file_left>0) done=0;
                    }
                }
                if(!done) continue;
                //发完，短连接直接关闭
                conn_close(c);
            }
        }
    }
}
