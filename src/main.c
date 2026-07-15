#include"server.h"
#include"config.h"
#include<stdio.h>
#include<signal.h>

static server_t *g_server=NULL;//信号处理器

static void on_signal(int sig){
    (void)sig;
    if(g_server){
        server_stop(g_server);//通知事件循环退出
    }
}

int main(int argc,char** argv){
    const char *conf_path=(argc>1)?argv[1]:"config.ini";

    config_t cfg;
    config_load(conf_path,&cfg);

    printf("[INFO]端口=%d,根目录=%s\n",cfg.port,cfg.root_dir);

    g_server=server_create(&cfg);
    if(g_server==NULL){
        fprintf(stderr,"[ERROR]服务器初始化失败\n");
        return 1;
    }

    signal(SIGINT,on_signal);//处理ctrl+C,停止当前的事件循环
    signal(SIGTERM,on_signal);//处理kill命令，停止当前的事件循环
    signal(SIGPIPE,SIG_IGN);//处理PIPE问题，忽略不管

    printf("[INFO]服务器启动,http://localhost:%d\n",cfg.port);
    server_run(g_server);

    server_destroy(g_server);
    printf("[INFO]服务器已关闭\n");
    return 0;
}