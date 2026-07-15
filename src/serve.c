#include "serve.h"
#include "config.h"
#include "http.h"
#include<sys/socket.h>
#include<facntl.h>

void serve(){
    int port,MAX_EVENTS,MAXLINE;
    get_cfg_int("MAX_EVENTS",&MAX_EVENTS);
    get_cfg_int("PORT",&port);
    get_cfg_int("MAXLINE",&MAXLINE);
    struct sockaddr_in servaddr,cliaddr;
    socklen_t cliaddr_len;
    int listenfd,connfd;
    if((listenfd=socket(AF_INET,SOCK_STREAM,0))<0){
        printf("socket creat failed\n");
        exit(1);
    }
    int op=1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&op,sizeof(op));
    bzero(&servaddr,sizeof(servaddr));
    servaddr.sin_family=AF_INET;
    servaddr.sin_addr.s_addr=htonl(INADDR_ANY);
    servaddr.sin_port=htons(port);
    if(bind(listenfd,(struct sockaddr *)&servaddr,sizeof(servaddr))<0){
        printf("bind address failed\n");
    }
    if(listen(listenfd,20)<0){
        printf("listen failed\n");
        exit(1);
    }
    fcntl(listenfd,F_SETFL,fcntl(listenfd,F_GETFL,0)|O_NONBLOCK);
    printf("connect successful!\n");
    int epfd=epoll_create1(0);
    if(epfd<0){
        printf("create connect failed\n");
        close(listenfd);
        exit(1);
    }
    struct epoll_event ev;
    ev.events=EPOLLIN;
    ev.data.fd=listenfd;
    if(epoll_ctl(epfd,EPOLL_CTL_ADD,listened,&ev)<0){
        printf("epoll_ctl add listened fail\n");
        close(listened);close(epfd);
        exit(1);
    }
    struct epoll_event events[MAX_EVENTS];
    while(1){
        int nready=epoll_wait(epfd,events,MAX_EVENTS,-1);
        if(nready<0){
            if(errno==EINTR)    continue;
            printf("epoll_wait error\n");
            break;
        }
        for(int i=0;i<nready;i++){
            int fd=events[i].data.fd;
            if(fd==listened){
                cliaddr_len=sizeof(cliaddr);
                int connfd=accept4(listened,(struct sockaddr *)&cliaddr,&cliaddr_len,SOCK_NONBLOCK);
                if(connfd<0){
                    printf("accept failed\n");
                    continue;
                }
                printf("received from %s at PORT %d\n",
                inet_ntop(AF_INET,&cliaddr.sin_addr,str,sizeof(str)),
                ntohs(cliaddr.sin_port));
                ev.events=EPOLLIN|EPOLLERR|EPOLLHUP;
                ev.data.fd=connfd;
                epoll_ctl(epfd,EPOLL_CTL_ADD,connfd,&ev);
            }
            else if(events[i].events&(EPOLLIN|EPOLLHUP|EPOLLERR))
                accept_request(fd);
        }
    }
}