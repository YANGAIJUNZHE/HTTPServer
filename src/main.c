#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "server.h"
#include "config.h"
#include "auth_db.h"

static int g_server;

static void on_signal(int sig) {
    (void)sig;
    if (g_server) {
        server_stop();
    }
}

int main(int argc, char **argv) {
    const char *cfg_path = (argc >= 2) ? argv[1] : "./config.ini";
    config_load(cfg_path);

    // 读 ROOT_DIR 用于数据库路径
    char root_dir[256];
    if (get_cfg_str("ROOT_DIR", root_dir) < 0) {
        fprintf(stderr, "ROOT_DIR config required\n");
        return 1;
    }

    // 转换 root_dir 为绝对路径（如果已经是绝对路径则原样用）
    char abs_root[512];
    if (root_dir[0] == '/') {
        strncpy(abs_root, root_dir, sizeof(abs_root)-1);
        abs_root[sizeof(abs_root)-1] = '\0';
    } else if (!realpath(root_dir, abs_root)) {
        // realpath 失败——可能配置文件路径需要相对于 CWD 解析
        // 尝试 chdir 到可执行文件所在目录后再解析
        fprintf(stderr, "Cannot resolve ROOT_DIR '%s', trying relative to CWD\n", root_dir);
        // 直接用 CWD + ROOT_DIR
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd))) {
            snprintf(abs_root, sizeof(abs_root), "%s/%s", cwd, root_dir);
            // realpath 验证
            char check[512];
            if (realpath(abs_root, check)) {
                strncpy(abs_root, check, sizeof(abs_root)-1);
                abs_root[sizeof(abs_root)-1] = '\0';
            }
        } else {
            fprintf(stderr, "Fatal: cannot resolve ROOT_DIR\n");
            return 1;
        }
    }

    // 初始化数据库（dirname(abs_root) + /data/users.db）
    if (auth_db_init(abs_root) < 0) {
        fprintf(stderr, "Database init failed\n");
        return 1;
    }

    // 创建服务器
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