#include <stdio.h>
#include <signal.h>
#include "server.h"
#include "config.h"
#include "auth_db.h"
#include "http.h"

static int g_server;

static void on_signal(int sig) {
    (void)sig;
    if (g_server) {
        server_stop();
    }
}

int main(int argc, char **argv) {
    const char *cfg_path = (argc >= 2) ? argv[1] : "../config.ini";
    config_load(cfg_path);

    // session 超时（秒）
    int session_timeout = 0;
    if (get_cfg_int("SESSION_TIMEOUT", &session_timeout) == 0 && session_timeout > 0)
        http_set_session_timeout(session_timeout);

    // 创建服务器（内部完成剩余配置读取、DB 初始化）
    g_server = server_create();
    if (!g_server) {
        fprintf(stderr, "server init failed\n");
        auth_db_close();
        return 1;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    server_run();

    auth_db_close();
    printf("[INFO] server shutdown complete\n");
    return 0;
}