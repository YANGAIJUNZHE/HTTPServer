#ifndef SERVER_H
#define SERVER_H

#include"config.h"

typedef struct server_t server_t;//不透明指针，保证封装性

server_t *server_create(const config_t *cfg);
void server_run(server_t *s);
void server_destroy(server_t *s);
void server_stop(server_t *s);//通知事件循环退出

#endif