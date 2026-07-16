# jdbhttpd — 轻量 HTTP 服务器

C 语言实现的单线程 epoll + 多线程大文件传输 HTTP 服务器，支持
用户认证（SQLite3）和动态页面。

## 快速开始

### 1. 安装依赖

项目依赖三个系统库的开发包：

| 库 | Ubuntu/Debian | Fedora/RHEL | Arch |
|---|---|---|---|
| SQLite3 | `sudo apt install libsqlite3-dev` | `sudo dnf install sqlite-devel` | `sudo pacman -S sqlite` |
| OpenSSL | `sudo apt install libssl-dev` | `sudo dnf install openssl-devel` | `sudo pacman -S openssl` |
| CMake | `sudo apt install cmake` | `sudo dnf install cmake` | `sudo pacman -S cmake` |

> pthread 是系统自带，无需额外安装。

### 2. 编译

```bash
cd HTTPServer
mkdir build && cd build
cmake ..
cmake --build .
```

### 3. 配置

编辑项目根目录的 `config.ini`：

```ini
PORT=38253          # 监听端口
ROOT_DIR=/绝对路径/www   # 静态文件目录（必须用绝对路径）
BACKLOG=128         # listen 队列长度
MAX_CONN=1024       # 最大并发连接数
TPOOL_THREADS=4     # 文件传输线程池大小
```

> `ROOT_DIR` 填写本机 `www/` 目录的绝对路径。数据库文件会自动创建在项目根目录的 `data/` 下。

### 4. 运行

```bash
cd HTTPServer
./build/web config.ini
```

按 `Ctrl+C` 优雅退出。

### 5. 首次使用

1. 浏览器打开 `http://localhost:38253/register.html` 注册账号
2. 登录后自动跳转到首页
3. 静态 HTML 文件放在 `www/` 目录下即可通过 HTTP 访问

## 项目结构

```
HTTPServer/
├── src/
│   ├── main.c          # 入口：信号处理、数据库初始化
│   ├── server.c/.h     # epoll 事件循环、时间轮、连接管理
│   ├── http.c/.h       # HTTP 解析（GET/POST）、Cookie、动态路由
│   ├── file.c/.h       # 静态文件服务、MIME 类型
│   ├── threadpool.c/.h # 多线程文件传输（>64KB 大文件）
│   ├── auth_db.c/.h    # SQLite3 用户认证、session、KV 数据
│   └── config.c/.h     # INI 配置读取
├── www/                # 文档根（静态 HTML + 登录/注册页）
├── data/               # 运行时自动创建，存放 SQLite 数据库
├── config.ini          # 服务器配置
└── CMakeLists.txt
```

## 架构要点

- **单线程 epoll 主循环**：accept → 读请求 → 解析 HTTP → 认证检查 → 发响应头
- **时间轮定时器**：512 槽 × 100ms 粒度，管理 keep-alive 空闲超时
- **固定连接池**：数组 `conns[MAX_CONNS]`，无动态分配
- **零拷贝传输**：`sendfile()`，小文件（<64KB）主线程直接发
- **线程池**：大文件（≥64KB）投递给工作线程，阻塞 `sendfile` 分块发送，完成后通过 `eventfd` 通知主线程
- **用户系统**：Cookie/Session 认证，SQLite3 存储，支持 KV 用户数据扩展

## 服务端 API 路由

| 路由 | 方法 | 认证 | 说明 |
|------|------|------|------|
| `/login.html` | GET | 否 | 登录页面 |
| `/register.html` | GET | 否 | 注册页面 |
| `/login` | POST | 否 | 登录：username + password → Set-Cookie → 302 |
| `/register` | POST | 否 | 注册：username + password |
| `/logout` | POST | 是 | 登出：清除 Cookie → 302 |
| `/*` | GET | 是 | 静态文件服务（未登录 → 302） |