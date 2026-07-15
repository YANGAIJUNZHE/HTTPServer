#include "file.h"

#include <stdio.h>    // snprintf
#include <stdlib.h>   // realpath
#include <string.h>   // strlen, strcasecmp, strrchr
#include <sys/stat.h> // fstat, S_ISREG
#include <fcntl.h>    // open, O_RDONLY
#include <unistd.h>   // close
#include <limits.h>   // PATH_MAX
#include <errno.h>    // errno

// MIME表：扩展名 → Content-Type
// strcasecmp 不区分大小写，兜底 application/octet-stream

static const struct
{
    const char *ext;
    const char *mime;
} mime_table[] = {
    {".html", "text/html; charset=utf-8"},
    {".htm", "text/html; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},
    {".js", "application/javascript; charset=utf-8"},
    {".json", "application/json; charset=utf-8"},
    {".txt", "text/plain; charset=utf-8"},
    {".xml", "application/xml; charset=utf-8"},
    {".svg", "image/svg+xml"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".png", "image/png"},
    {".gif", "image/gif"},
    {".ico", "image/x-icon"},
    {".webp", "image/webp"},
    {".pdf", "application/pdf"},
    {NULL, "application/octet-stream"}, // 兜底
};

static const char *mime_by_path(const char *path)
{
    const char *dot = strrchr(path, '.'); // 找最后一个 '.'
    if (!dot)
        return "application/octet-stream";

    for (int i = 0; mime_table[i].ext; i++)
    {
        if (strcasecmp(dot, mime_table[i].ext) == 0)
        {
            return mime_table[i].mime;
        }
    }
    return "application/octet-stream";
}

// URL 解码：将 %XX 转换为对应字节，原地修改，返回解码后长度
static size_t url_decode(char *s, size_t len)
{
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

// 检查缓冲区中是否包含 ".."（路径穿越检测）
static int contains_dotdot(const char *s, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++)
        if (s[i] == '.' && s[i+1] == '.') return 1;
    return 0;
}

// file_serve 是唯一对外接口
// URI → 磁盘文件的完整映射流程：
//   1. url_decode(URI) → 把浏览器百分号编码还原为真实文件名
//   2. ".." 检测 → 防止路径穿越攻击
//   3. 拼接 root_real + 解码后 URI → raw_path
//   4. realpath(raw_path) → 安全验证（先验证再 open）
//   5. 前缀校验（safe_real 必须在 root_real 内）
//   6. open + fstat（fstat 消除 TOCTOU）
//   7. 后缀名 → MIME 查表
//
// root_real: 服务器启动时 realpath(root_dir) 一次，之后复用
// root_len:  strlen(root_real)，避免每次请求都算

int file_serve(const char *uri, size_t uri_len,
               const char *root_real, size_t root_len,
               int *file_fd, off_t *size,
               const char **mime, int *status)
{
    char raw_path[PATH_MAX];
    char safe_real[PATH_MAX];
    struct stat st;
    int fd;

    // 1. URL 解码 + ".." 检测（解码后检测）
    char decoded_uri[PATH_MAX];
    if (uri_len >= sizeof(decoded_uri)) { *status = 400; return -1; }
    memcpy(decoded_uri, uri, uri_len);
    decoded_uri[uri_len] = '\0';
    size_t decoded_len = url_decode(decoded_uri, uri_len);
    decoded_uri[decoded_len] = '\0';
    if (contains_dotdot(decoded_uri, decoded_len))
    {
        *status = 400;
        return -1;
    }

    // 2. 拼接根目录 + 解码后的 URI
    int need;
    if (decoded_len > 0 && decoded_uri[decoded_len-1] == '/') {
        need = snprintf(raw_path, sizeof(raw_path), "%s/%sindex.html",
                        root_real, decoded_uri);
    } else {
        need = snprintf(raw_path, sizeof(raw_path), "%s/%s",
                        root_real, decoded_uri);
    }
    if (need < 0 || (size_t)need >= sizeof(raw_path))
    {
        *status = 400;
        return -1;
    }

    // 3. realpath 安全验证
    if (realpath(raw_path, safe_real) == NULL)
    {
        *status = (errno == ENOENT) ? 404 : 400;
        return -1;
    }

    // 4. 前缀校验：safe_real 必须在 root_real 内
    if (strncmp(safe_real, root_real, root_len) != 0)
    {
        *status = 400; // 解析后在根目录外
        return -1;
    }
    // 防前缀欺骗：/var/www 不能匹配 /var/www-evil
    if (safe_real[root_len] != '\0' && safe_real[root_len] != '/')
    {
        *status = 400;
        return -1;
    }

    // 5. open — 路径已验证安全
    fd = open(raw_path, O_RDONLY|O_NOFOLLOW);
    if (fd == -1)
    {
        *status = (errno == ENOENT) ? 404 : 400;
        return -1;
    }

    // 6. fstat — 拿已打开 fd 的元数据，消除 TOCTOU
    if (fstat(fd, &st) == -1)
    {
        *status = 400;
        close(fd);
        return -1;
    }
    if (!S_ISREG(st.st_mode))
    {
        *status = 404; // 目录或设备 → 当作不存在
        close(fd);
        return -1;
    }
    *size = st.st_size;

    // 7. MIME — 用 safe_real，不依赖 readlink
    *mime = mime_by_path(safe_real);
    *file_fd = fd;
    *status = 200;
    return 0;
}
