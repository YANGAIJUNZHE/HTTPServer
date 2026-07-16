#include "http.h"
#include "file.h"
#include "auth_db.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>

char g_root[256];

// --- URL 解码 ---
static size_t url_decode(char *s, size_t len) {
    size_t i = 0, j = 0;
    while (i < len) {
        if (s[i] == '%' && i + 2 < len) {
            char hex[3] = {s[i+1], s[i+2], '\0'};
            char *end;
            long val = strtol(hex, &end, 16);
            if (end == hex + 2) {
                s[j++] = (char)val;
                i += 3;
                continue;
            }
        }
        s[j++] = s[i++];
    }
    return j;
}

// --- Cookie 解析：从请求头提取指定 cookie 值到 buf ---
static int get_cookie(const char *headers, const char *name, char *buf, size_t bufsz) {
    const char *ck = strcasestr(headers, "Cookie:");
    if (!ck) return -1;

    // 移到 Cookie: 值开始
    ck += 7;
    while (*ck == ' ') ck++;

    const char *line_end = strstr(ck, "\r\n");
    if (!line_end) line_end = ck + strlen(ck);

    // 在 cookie 行内找 name=
    size_t nlen = strlen(name);
    const char *p = ck;
    while (p < line_end) {
        // 跳过前导空格和分号空格
        while (p < line_end && (*p == ' ' || *p == ';')) p++;
        if (p >= line_end) break;

        if (strncasecmp(p, name, nlen) == 0 && p[nlen] == '=') {
            p += nlen + 1;
            size_t i = 0;
            while (p < line_end && *p != ';' && *p != ' ' && i < bufsz - 1) {
                buf[i++] = *p++;
            }
            buf[i] = '\0';
            return 0;
        }
        // 跳到下一个分号
        while (p < line_end && *p != ';') p++;
    }
    return -1;
}

// --- URL 参数解析：从 query string 或 body 提取参数 ---
static int get_param(const char *body, const char *name, char *buf, size_t bufsz) {
    size_t nlen = strlen(name);
    const char *p = body;
    const char *end = body + strlen(body);

    while (p < end) {
        // 找 name= 或 &name=
        const char *eq = strchr(p, '=');
        if (!eq) break;

        // 检查 eq 前面是否是 name
        const char *key_start = eq - 1;
        while (key_start >= p && *key_start != '&') key_start--;
        key_start++;  // 回到 key 第一个字符

        if ((size_t)(eq - key_start) == nlen &&
            strncasecmp(key_start, name, nlen) == 0) {
            // 提取值
            const char *v = eq + 1;
            size_t i = 0;
            while (v < end && *v != '&' && i < bufsz - 1) {
                buf[i++] = *v++;
            }
            buf[i] = '\0';

            // URL decode 这个值
            size_t dlen = url_decode(buf, i);
            buf[dlen] = '\0';
            return 0;
        }
        p = eq + 1;
    }
    return -1;
}

// --- 解析请求行 ---
static int parse_request_line(const char *buf, char *method, char *path, char *query) {
    const char *end = strstr(buf, "\r\n");
    if (!end) return 400;

    int i = 0, j = 0;

    // 解析方法
    while (buf[i] != '\0' && !isspace((unsigned char)buf[i]) && j < 15) {
        method[j++] = buf[i++];
    }
    method[j] = '\0';

    // 检查方法（支持 GET 和 POST）
    if (strcasecmp(method, "GET") && strcasecmp(method, "POST"))
        return 501;

    // 跳过空白
    while (buf[i] != '\0' && isspace((unsigned char)buf[i])) i++;
    if (buf[i] == '\0') return 400;

    // 解析 path + query
    j = 0;
    query[0] = '\0';
    while (buf[i] != '\0' && !isspace((unsigned char)buf[i]) && j < 255) {
        char c = buf[i++];
        if (c == '?') {
            // 后续是 query string
            path[j] = '\0';
            int qj = 0;
            while (buf[i] != '\0' && !isspace((unsigned char)buf[i]) && qj < 511) {
                query[qj++] = buf[i++];
            }
            query[qj] = '\0';
            goto done_path;
        }
        path[j++] = c;
    }
    path[j] = '\0';
done_path:

    j = (int)url_decode(path, j);
    path[j] = '\0';

    if (strstr(path, "..")) return 400;
    // 默认首页
    if (strcmp(path, "/") == 0)
        strcpy(path, "/index.html");

    return 200;
}

// --- 提取 POST body ---
static const char *find_body(const char *buf) {
    const char *sep = strstr(buf, "\r\n\r\n");
    if (!sep) return NULL;
    return sep + 4;
}

// --- 构建 HTML 响应 ---
static int build_html_response(int code, const char *title, const char *body_html,
                                char *hdr, size_t hdr_size) {
    char page[2048];
    int page_len = snprintf(page, sizeof(page),
        "<!DOCTYPE html>\r\n"
        "<html><head><meta charset=\"utf-8\">"
        "<title>%s</title>"
        "<style>body{font-family:sans-serif;max-width:600px;margin:40px auto;padding:20px}"
        ".msg{background:#f0f0f0;padding:16px;border-radius:8px}"
        "a{color:#06c}</style></head>\r\n"
        "<body><h2>%s</h2><div class=\"msg\">%s</div></body></html>\r\n",
        title, title, body_html);

    const char *status = code == 200 ? "200 OK" :
                          code == 302 ? "302 Found" :
                          code == 400 ? "400 Bad Request" :
                          code == 401 ? "401 Unauthorized" :
                          code == 404 ? "404 Not Found" : "500 Internal Server Error";

    return snprintf(hdr, hdr_size,
        "HTTP/1.1 %s\r\n"
        "Server: jdbhttpd/0.2.0\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status, page_len, page);
}

// --- 构建重定向响应 ---
static int build_redirect(int code, const char *location, const char *set_cookie,
                           char *hdr, size_t hdr_size) {
    int len = snprintf(hdr, hdr_size,
        "HTTP/1.1 %d %s\r\n"
        "Server: jdbhttpd/0.2.0\r\n"
        "Location: %s\r\n",
        code, code == 302 ? "Found" : "See Other", location);

    if (set_cookie && set_cookie[0]) {
        len += snprintf(hdr + len, hdr_size - (size_t)len,
            "Set-Cookie: %s\r\n", set_cookie);
    }

    len += snprintf(hdr + len, hdr_size - (size_t)len,
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n");

    return len;
}

// --- 处理登录 ---
static int handle_login(const char *body, struct response *resp) {
    char username[64] = {0}, password[64] = {0};

    if (get_param(body, "username", username, sizeof(username)) < 0 ||
        get_param(body, "password", password, sizeof(password)) < 0 ||
        username[0] == '\0' || password[0] == '\0') {
        resp->file_fd = -1;
        resp->file_size = 0;
        resp->hdr_len = build_html_response(400, "Login Failed",
            "<p>Missing username or password.</p><p><a href=\"/login.html\">Try again</a></p>",
            resp->hdr, sizeof(resp->hdr));
        resp->keep_alive = 0;
        resp->use_thread = 0;
        resp->notify_fd = -1;
        return 200;
    }

    int user_id = auth_verify(username, password);
    if (user_id <= 0) {
        resp->file_fd = -1;
        resp->file_size = 0;
        resp->hdr_len = build_html_response(401, "Login Failed",
            "<p>Invalid username or password.</p><p><a href=\"/login.html\">Try again</a></p>",
            resp->hdr, sizeof(resp->hdr));
        resp->keep_alive = 0;
        resp->use_thread = 0;
        resp->notify_fd = -1;
        return 200;
    }

    // 创建 session（2 小时有效）
    char *token = auth_session_create(user_id, 7200);
    if (!token) {
        resp->file_fd = -1;
        resp->file_size = 0;
        resp->hdr_len = build_html_response(500, "Error",
            "<p>Failed to create session. Please try again.</p>",
            resp->hdr, sizeof(resp->hdr));
        resp->keep_alive = 0;
        resp->use_thread = 0;
        resp->notify_fd = -1;
        return 200;
    }

    char set_cookie[96];
    snprintf(set_cookie, sizeof(set_cookie),
        "session=%s; Path=/; HttpOnly; Max-Age=7200", token);
    free(token);

    resp->file_fd = -1;
    resp->file_size = 0;
    resp->hdr_len = build_redirect(302, "/index.html", set_cookie,
                                    resp->hdr, sizeof(resp->hdr));
    resp->keep_alive = 0;
    resp->use_thread = 0;
    resp->notify_fd = -1;
    return 200;
}

// --- 处理登出 ---
static int handle_logout(struct response *resp) {
    char set_cookie[64];
    snprintf(set_cookie, sizeof(set_cookie),
        "session=; Path=/; HttpOnly; Max-Age=0");

    resp->file_fd = -1;
    resp->file_size = 0;
    resp->hdr_len = build_redirect(302, "/login.html", set_cookie,
                                    resp->hdr, sizeof(resp->hdr));
    resp->keep_alive = 0;
    resp->use_thread = 0;
    resp->notify_fd = -1;
    return 200;
}

// --- 处理注册 ---
static int handle_register(const char *body, struct response *resp) {
    char username[64] = {0}, password[64] = {0};

    if (get_param(body, "username", username, sizeof(username)) < 0 ||
        get_param(body, "password", password, sizeof(password)) < 0 ||
        username[0] == '\0' || password[0] == '\0') {
        resp->file_fd = -1;
        resp->file_size = 0;
        resp->hdr_len = build_html_response(400, "Registration Failed",
            "<p>Missing username or password.</p><p><a href=\"/register.html\">Try again</a></p>",
            resp->hdr, sizeof(resp->hdr));
        resp->keep_alive = 0;
        resp->use_thread = 0;
        resp->notify_fd = -1;
        return 200;
    }

    int user_id = auth_register(username, password);
    if (user_id <= 0) {
        resp->file_fd = -1;
        resp->file_size = 0;
        resp->hdr_len = build_html_response(400, "Registration Failed",
            "<p>Username may already exist.</p><p><a href=\"/register.html\">Try again</a></p>",
            resp->hdr, sizeof(resp->hdr));
        resp->keep_alive = 0;
        resp->use_thread = 0;
        resp->notify_fd = -1;
        return 200;
    }

    resp->file_fd = -1;
    resp->file_size = 0;
    resp->hdr_len = build_html_response(200, "Registration Successful",
        "<p>Account created! <a href=\"/login.html\">Click here to login</a>.</p>",
        resp->hdr, sizeof(resp->hdr));
    resp->keep_alive = 0;
    resp->use_thread = 0;
    resp->notify_fd = -1;
    return 200;
}

// --- 主请求处理 ---
int accept_request(char *rbuf, int *rlen, struct response *resp) {
    // 找头部结束 \r\n\r\n
    char *end = strstr(rbuf, "\r\n\r\n");
    if (!end) return 0;  // 数据不完整

    int consumed = (int)(end - rbuf) + 4;

    // 默认值
    resp->keep_alive = 0;
    resp->use_thread = 0;
    resp->notify_fd = -1;

    // 解析请求行
    char method[16], path[256], query[512] = {0};
    int ret = parse_request_line(rbuf, method, path, query);
    if (ret != 200) {
        resp->file_fd = -1;
        resp->file_size = 0;
        const char *msg = (ret == 400) ? "Bad Request" :
                          (ret == 404) ? "Not Found" : "Not Implemented";
        resp->hdr_len = snprintf(resp->hdr, sizeof(resp->hdr),
            "HTTP/1.1 %d %s\r\n"
            "Server: jdbhttpd/0.2.0\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<HTML><TITLE>%d %s</TITLE>\r\n"
            "<BODY><P>%d %s</P></BODY></HTML>\r\n",
            ret, msg, ret, msg, ret, msg);
        goto consume;
    }

    // ---- 动态路由处理（POST） ----
    if (strcasecmp(method, "POST") == 0) {
        const char *body = find_body(rbuf);
        if (!body) {
            // 没有 body，但也算完整请求
            resp->file_fd = -1;
            resp->file_size = 0;
            resp->hdr_len = build_html_response(400, "Bad Request",
                "<p>POST without body.</p>", resp->hdr, sizeof(resp->hdr));
            goto consume;
        }

        if (strcmp(path, "/login") == 0) {
            handle_login(body, resp);
            goto consume;
        }
        if (strcmp(path, "/register") == 0) {
            handle_register(body, resp);
            goto consume;
        }
        if (strcmp(path, "/logout") == 0) {
            handle_logout(resp);
            goto consume;
        }

        // 未知 POST 路径 → 404
        resp->file_fd = -1;
        resp->file_size = 0;
        resp->hdr_len = build_html_response(404, "Not Found",
            "<p>The requested resource was not found.</p>", resp->hdr, sizeof(resp->hdr));
        goto consume;
    }

    // ---- GET 请求：认证检查 ----
    // 登录页和注册页不需要认证
    int is_public = (strcmp(path, "/login.html") == 0 ||
                     strcmp(path, "/register.html") == 0);

    if (!is_public) {
        char session[128] = {0};
        int has_session = (get_cookie(rbuf, "session", session, sizeof(session)) == 0);

        if (!has_session || auth_session_check(session) <= 0) {
            // 未登录 → 重定向到登录页
            resp->file_fd = -1;
            resp->file_size = 0;
            resp->hdr_len = build_redirect(302, "/login.html", NULL,
                                            resp->hdr, sizeof(resp->hdr));
            goto consume;
        }
    }

    // ---- 静态文件服务 ----
    ret = prepare_response(path, g_root, resp);
    if (ret != 200) {
        resp->file_fd = -1;
        resp->file_size = 0;
        const char *msg = (ret == 404) ? "Not Found" : "URI Too Long";
        resp->hdr_len = snprintf(resp->hdr, sizeof(resp->hdr),
            "HTTP/1.1 %d %s\r\n"
            "Server: jdbhttpd/0.2.0\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<title>%d %s</title></head>\r\n"
            "<body><h1>%d %s</h1></body></html>\r\n",
            ret, msg, ret, msg, ret, msg);
        goto consume;
    }

    // 静态文件：大文件（>64KB）使用线程池，其他主线程直接 sendfile
    // keep-alive 模式，发完后连接保持等待下一个请求
    resp->keep_alive = 1;
    if (resp->file_size > 64 * 1024) {
        resp->use_thread = 1;
    } else {
        resp->use_thread = 0;
    }

consume:
    // 排空已消费的头部
    memmove(rbuf, rbuf + consumed, (size_t)(*rlen - consumed));
    *rlen -= consumed;
    return 1;
}