#include "auth_db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <time.h>
#include <openssl/rand.h>

static sqlite3 *g_db = NULL;

// --- 内部工具 ---

static int exec_sql(const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB] SQL error: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

// 生成随机 hex token（64 字符 = 256 位熵）
static char *gen_token(void) {
    unsigned char raw[32];
    if (!RAND_bytes(raw, sizeof(raw))) return NULL;

    char *hex = malloc(65);
    if (!hex) return NULL;
    for (int i = 0; i < 32; i++) {
        sprintf(hex + i * 2, "%02x", raw[i]);
    }
    hex[64] = '\0';
    return hex;
}

// --- 初始化 ---

int auth_db_init(const char *root_dir) {
    // root_dir 现在是绝对路径（如 /home/yang/HTTPServer/www）
    // 数据库放在 root_dir 的上层目录的 data/ 下
    char db_path[512];

    // 如果 root_dir 已是绝对路径，取其 dirname 后拼 data/
    // 若 root_dir 以 /www 结尾，去掉它
    char dir[512];
    strncpy(dir, root_dir, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    // 去掉末尾的路径分量（即去掉 /www，保留项目根路径）
    char *last_slash = strrchr(dir, '/');
    if (last_slash && last_slash != dir) {
        *last_slash = '\0';
    }
    snprintf(db_path, sizeof(db_path), "%s/data/users.db", dir);

    int rc = sqlite3_open(db_path, &g_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB] Cannot open %s: %s\n", db_path, sqlite3_errmsg(g_db));
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    // 建表
    const char *schema =
        "CREATE TABLE IF NOT EXISTS users ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username  TEXT    NOT NULL UNIQUE,"
        "  password  TEXT    NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  token     TEXT    PRIMARY KEY,"
        "  user_id   INTEGER NOT NULL,"
        "  created   INTEGER NOT NULL,"
        "  expires   INTEGER NOT NULL,"
        "  FOREIGN KEY(user_id) REFERENCES users(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS user_data ("
        "  user_id   INTEGER NOT NULL,"
        "  key       TEXT    NOT NULL,"
        "  value     TEXT    NOT NULL DEFAULT '',"
        "  PRIMARY KEY(user_id, key),"
        "  FOREIGN KEY(user_id) REFERENCES users(id)"
        ");";

    if (exec_sql(schema) < 0) {
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    printf("[DB] database initialized at %s\n", db_path);
    return 0;
}

// --- 用户认证 ---

int auth_verify(const char *username, const char *password) {
    if (!g_db) return 0;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, password FROM users WHERE username = ?";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    int user_id = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *stored_pw = (const char *)sqlite3_column_text(stmt, 1);
        // 简单明文比较——生产应用该用 bcrypt/scrypt
        if (stored_pw && strcmp(password, stored_pw) == 0) {
            user_id = sqlite3_column_int(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    return user_id;
}

// --- Session ---

char *auth_session_create(int user_id, uint64_t timeout_sec) {
    if (!g_db) return NULL;

    char *token = gen_token();
    if (!token) return NULL;

    time_t now = time(NULL);
    time_t expires = now + (time_t)timeout_sec;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO sessions(token, user_id, created, expires) VALUES(?,?,?,?)";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(token);
        return NULL;
    }
    sqlite3_bind_text(stmt, 1, token, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt,  2, user_id);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)now);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)expires);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        free(token);
        return NULL;
    }
    sqlite3_finalize(stmt);
    return token;
}

int auth_session_check(const char *token) {
    if (!g_db || !token) return 0;

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT user_id, expires FROM sessions WHERE token = ?";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;

    sqlite3_bind_text(stmt, 1, token, -1, SQLITE_TRANSIENT);

    int user_id = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        time_t expires = (time_t)sqlite3_column_int64(stmt, 1);
        if (time(NULL) < expires) {
            user_id = sqlite3_column_int(stmt, 0);
        } else {
            // 过期，清理
            sqlite3_finalize(stmt);
            stmt = NULL;
            const char *del = "DELETE FROM sessions WHERE token = ?";
            sqlite3_prepare_v2(g_db, del, -1, &stmt, NULL);
            sqlite3_bind_text(stmt, 1, token, -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
        }
    }
    sqlite3_finalize(stmt);
    return user_id;
}

// --- 注册 ---

int auth_register(const char *username, const char *password) {
    if (!g_db) return 0;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO users(username, password) VALUES(?,?)";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password, -1, SQLITE_STATIC);

    int user_id = 0;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        user_id = (int)sqlite3_last_insert_rowid(g_db);
    }
    sqlite3_finalize(stmt);
    return user_id;
}

// --- 用户数据 ---

int auth_user_data_get(int user_id, const char *key, char *value, size_t maxlen) {
    if (!g_db) return -1;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT value FROM user_data WHERE user_id = ? AND key = ?";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);

    int rc = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(stmt, 0);
        if (v) {
            strncpy(value, v, maxlen - 1);
            value[maxlen - 1] = '\0';
            rc = 0;
        }
    }
    sqlite3_finalize(stmt);
    return rc;
}

int auth_user_data_set(int user_id, const char *key, const char *value) {
    if (!g_db) return -1;

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT OR REPLACE INTO user_data(user_id, key, value) VALUES(?,?,?)";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, value, -1, SQLITE_STATIC);

    int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

// --- 关闭 ---

void auth_db_close(void) {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
        printf("[DB] database closed\n");
    }
}