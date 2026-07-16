#ifndef AUTH_DB_H
#define AUTH_DB_H

#include <stddef.h>
#include <stdint.h>

/* 初始化 / 关闭 */
int  auth_db_init(const char *root_dir);
void auth_db_close(void);

/* 用户 */
int  auth_register(const char *username, const char *password);
int  auth_verify(const char *username, const char *password);

/* Session */
char *auth_session_create(int user_id, uint64_t timeout_sec);
int   auth_session_check(const char *token);
void  auth_session_delete(const char *token);

#endif
