#include <stdio.h>
#include <signal.h>
#include "server.h"

static int g_server;//信号处理器

static void on_signal(int sig){
    (void)sig;
    if(g_server){
        server_stop();//通知事件循环退出
    }
}

int main(void){
    g_server=server_create();
    if(!g_server){
        fprintf(stderr,"server init failed\n");
        return 1;
    }
    signal(SIGINT,on_signal);//处理ctrl+C,停止当前的事件循环
    signal(SIGTERM,on_signal);//处理kill命令，停止当前的事件循环
    signal(SIGPIPE,SIG_IGN);//处理PIPE问题，忽略不管
    server_run();
    printf("[INFO]服务器已关闭\n");
}