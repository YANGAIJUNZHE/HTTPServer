#ifndef FILE_H
#define FILE_H

#include <stddef.h>    // size_t
#include <sys/types.h> // off_t

// URI → 磁盘文件映射
// root_real: 服务器启动时 realpath(root_dir) 的结果（复用，不每次调 realpath）
// root_len:  strlen(root_real)
// 成功返回 0，填充 *file_fd/*size/*mime
// 失败返回 -1，填充 *status（400 或 404）

int file_serve(const char *uri, size_t uri_len,
               const char *root_real, size_t root_len,
               int *file_fd, off_t *size,
               const char **mime, int *status);

#endif
