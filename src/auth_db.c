#include "auth_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sqlite3.h>
#include <time.h>
#include <openssl/rand.h>

static sqlite3 *g_db = NULL;

/* ------------------------------------------------------------------ */
/*  内部工具                                                           */
/* ------------------------------------------------------------------ */

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

static char *gen_token(void) {
    unsigned char raw[32];
    if (!RAND_bytes(raw, sizeof(raw))) return NULL;

    char *hex = malloc(65);
    if (!hex) return NULL;
    for (int i = 0; i < 32; i++)
        sprintf(hex + i * 2, "%02x", raw[i]);
    hex[64] = '\0';
    return hex;
}

/* ------------------------------------------------------------------ */
/*  初始化 / 关闭                                                      */
/* ------------------------------------------------------------------ */

int auth_db_init(const char *root_dir) {
    char dir[512];
    char db_path[540];  // dir(512) + "/data/users.db"(14) + '\0'

    strncpy(dir, root_dir, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    // root_dir 末级目录名去掉，取上一级作为项目根
    char *last_slash = strrchr(dir, '/');
    if (last_slash && last_slash != dir)
        *last_slash = '\0';

    // 确保 data/ 目录存在
    char data_dir[540];
    snprintf(data_dir, sizeof(data_dir), "%s/data", dir);
    mkdir(data_dir, 0755);

    snprintf(db_path, sizeof(db_path), "%s/data/users.db", dir);

    int rc = sqlite3_open(db_path, &g_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB] Cannot open %s: %s\n", db_path, sqlite3_errmsg(g_db));
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    const char *schema =
        "CREATE TABLE IF NOT EXISTS users ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username  TEXT    NOT NULL UNIQUE,"
        "  password  TEXT    NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  token     TEXT    PRIMARY KEY,"
        "  user_id   INTEGER NOT NULL,"
        "  expires   INTEGER NOT NULL,"
        "  FOREIGN KEY(user_id) REFERENCES users(id)"
        ");";

    if (exec_sql(schema) < 0) {
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    printf("[DB] initialized at %s\n", db_path);
    return 0;
}

void auth_db_close(void) {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
        printf("[DB] closed\n");
    }
}

/* ------------------------------------------------------------------ */
/*  用户                                                               */
/* ------------------------------------------------------------------ */

int auth_register(const char *username, const char *password) {
    if (!g_db) return 0;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO users(username, password) VALUES(?,?)";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password, -1, SQLITE_STATIC);

    int user_id = 0;
    if (sqlite3_step(stmt) == SQLITE_DONE)
        user_id = (int)sqlite3_last_insert_rowid(g_db);

    sqlite3_finalize(stmt);
    return user_id;
}

int auth_verify(const char *username, const char *password) {
    if (!g_db) return 0;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, password FROM users WHERE username = ?";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    int user_id = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *stored = (const char *)sqlite3_column_text(stmt, 1);
        if (stored && strcmp(password, stored) == 0)
            user_id = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return user_id;
}

/* ------------------------------------------------------------------ */
/*  Session                                                            */
/* ------------------------------------------------------------------ */

char *auth_session_create(int user_id, uint64_t timeout_sec) {
    if (!g_db) return NULL;

    char *token = gen_token();
    if (!token) return NULL;

    time_t expires = time(NULL) + (time_t)timeout_sec;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO sessions(token, user_id, expires) VALUES(?,?,?)";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(token);
        return NULL;
    }

    sqlite3_bind_text(stmt,  1, token,   -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt,   2, user_id);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)expires);

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
    const char *sql = "SELECT user_id, expires FROM sessions WHERE token = ?";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_text(stmt, 1, token, -1, SQLITE_TRANSIENT);

    int user_id = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        time_t expires = (time_t)sqlite3_column_int64(stmt, 1);
        if (time(NULL) < expires) {
            user_id = sqlite3_column_int(stmt, 0);
        } else {
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

void auth_session_delete(const char *token) {
    if (!g_db || !token) return;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "DELETE FROM sessions WHERE token = ?";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return;

    sqlite3_bind_text(stmt, 1, token, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}
