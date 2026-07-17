#!/bin/bash
# HTTPServer 测试脚本
# 用法：cd /home/yang/HTTPServer && bash test.sh

set -e
HOST="http://localhost:38253"
PASS=0
FAIL=0

green() { echo -e "\033[32m$1\033[0m"; }
red()   { echo -e "\033[31m$1\033[0m"; }
log()   { echo "[$(date +%H:%M:%S)] $1"; }

# ── 确保服务器在运行 ──
if ! curl -s -o /dev/null "$HOST/login.html" 2>/dev/null; then
    red "服务器未启动，请先运行: ./build/web config.ini &"
    exit 1
fi

# ── 1. 注册 + 登录获取 token ──
log "=== 第1步: 注册账号 ==="
USER="perftest_$(date +%s)"
curl -s -X POST "$HOST/register" -d "username=$USER&password=test123" > /dev/null
green "注册成功: $USER"

log "=== 第2步: 登录获取 token ==="
curl -s -c /tmp/test_cookie.txt -X POST "$HOST/login" -d "username=$USER&password=test123" > /dev/null
TOKEN=$(grep session /tmp/test_cookie.txt | awk '{print $NF}')
if [ -z "$TOKEN" ]; then
    red "登录失败"
    exit 1
fi
green "Token: ${TOKEN:0:16}..."

# ── 2. 大文件 MD5 完整性 ──
log "=== 测试1: 10MB 文件 MD5 完整性 ==="
dd if=/dev/urandom of=www/test_10m.bin bs=1M count=10 2>/dev/null
SRC=$(md5sum www/test_10m.bin | awk '{print $1}')
DL=$(curl -s -b /tmp/test_cookie.txt "$HOST/test_10m.bin" | md5sum | awk '{print $1}')

if [ "$SRC" = "$DL" ]; then
    green "[PASS] MD5 一致: $SRC"
    PASS=$((PASS + 1))
else
    red "[FAIL] 源: $SRC  下载: $DL"
    FAIL=$((FAIL + 1))
fi
rm -f www/test_10m.bin

# ── 3. 并发小文件测试 ──
log "=== 测试2: 200 次小文件请求 ==="
SUCCESS=0
for i in $(seq 1 200); do
    code=$(curl -s -o /dev/null -w '%{http_code}' "$HOST/login.html")
    [ "$code" = "200" ] && SUCCESS=$((SUCCESS + 1))
done
if [ $SUCCESS -eq 200 ]; then
    green "[PASS] 200/200 全部成功"
    PASS=$((PASS + 1))
else
    red "[FAIL] $SUCCESS/200 成功"
    FAIL=$((FAIL + 1))
fi

# ── 4. 并发大文件下载 + 完整性 ──
log "=== 测试3: 5 并发下载 1MB 文件 ==="
dd if=/dev/urandom of=www/perf_1m.bin bs=1M count=1 2>/dev/null
SRC=$(md5sum www/perf_1m.bin | awk '{print $1}')

for i in 1 2 3 4 5; do
    curl -s -b /tmp/test_cookie.txt -o /tmp/dl_${i}.bin "$HOST/perf_1m.bin" &
done
wait

ALL_OK=1
for i in 1 2 3 4 5; do
    DL=$(md5sum /tmp/dl_${i}.bin | awk '{print $1}')
    if [ "$SRC" != "$DL" ]; then
        red "[FAIL] 文件 $i MD5 不一致: $DL"
        ALL_OK=0
        FAIL=$((FAIL + 1))
    fi
done
if [ $ALL_OK -eq 1 ]; then
    green "[PASS] 5 个并发下载 MD5 全部一致"
    PASS=$((PASS + 1))
fi

rm -f www/perf_1m.bin /tmp/dl_*.bin

# ── 结果汇总 ──
echo ""
echo "=============================="
echo "  测试结果: $PASS 通过, $FAIL 失败"
echo "=============================="
[ $FAIL -eq 0 ] && green "全部通过!" || red "有问题，请检查"