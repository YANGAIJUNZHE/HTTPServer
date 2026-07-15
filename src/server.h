#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>

struct conn;

typedef struct wtimer {
    struct conn *conn;
    uint64_t    expire;
    int         slot;
    struct wtimer *prev;
    struct wtimer *next;
} wtimer_t;

void    timer_init(void);
void    timer_add(struct conn *c, uint64_t timeout_ms);
void    timer_cancel(struct conn *c);
void    timer_tick(void);
uint64_t timer_now_ms(void);

int server_create(void);
void server_stop(void);
void server_run(void);

#endif