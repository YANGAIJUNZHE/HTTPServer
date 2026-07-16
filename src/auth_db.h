#ifndef AUTH_DB_H
#define AUTH_DB_H

#include <stddef.h>
#include <stdint.h>

// 初始化数据库（建表等），root_dir 用于定位 db 文件
int auth_db_init(const char *root_dir);

// 验证用户名密码，返回 user_id（>0），失败返回 0
int auth_verify(const char *username, const char *password);

// 创建 session，返回 token（需调用方 free），失败返回 NULL
char *auth_session_create(int user_id, uint64_t timeout_sec);

// 验证 session token，返回 user_id（>0），过期/无效返回 0
int auth_session_check(const char *token);

// 用户注册（返回 user_id，失败返回 0）
int auth_register(const char *username, const char *password);

// 关闭数据库
void auth_db_close(void);

// 获取用户数据（通用 KV）
int auth_user_data_get(int user_id, const char *key, char *value, size_t maxlen);

// 设置用户数据
int auth_user_data_set(int user_id, const char *key, const char *value);

#endif