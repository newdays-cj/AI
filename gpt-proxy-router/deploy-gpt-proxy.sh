#!/usr/bin/env bash
# ============================================================
# GPT 智能路由代理 - 一键部署脚本
#
# 功能:
#   - OpenAI 相关域名 (api.openai.com / chatgpt.com 等) 自动走 SOCKS5 代理
#   - 其他所有流量直连, 不污染全局网络
#   - 部署后所有程序 (curl / python / opencode / git ...) 零特殊命令直接可用
#
# 组成:
#   1. ~/.local/bin/gpt-proxy-router.py   本地智能路由程序 (纯Python标准库)
#   2. systemd 用户服务 (无systemd时降级为 nohup + .bashrc 自启)
#   3. ~/.bashrc 代理环境变量 (幂等, 可重复执行)
#
# 用法:
#   bash deploy-gpt-proxy.sh           # 部署
#   bash deploy-gpt-proxy.sh --uninstall  # 卸载
#
# 部署完成后: 重新打开终端 (或 source ~/.bashrc), 重启需要联网的应用即可
# ============================================================
set -u

# ================= 配置区 (按需修改) =================
UPSTREAM_HOST=""                    # 必填: 上游 SOCKS5 代理地址
UPSTREAM_PORT="1080"                # 上游 SOCKS5 代理端口
UPSTREAM_USER=""                    # 必填: 上游代理用户名
UPSTREAM_PASS=""                    # 必填: 上游代理密码
LISTEN_HOST="127.0.0.1"             # 本地路由监听地址 (不要改)
LISTEN_PORT="17890"                 # 本地路由监听端口 (冲突时改这里)
# =====================================================

HOME_DIR="${HOME:?HOME环境变量不存在, 请用普通用户运行}"
BIN_DIR="$HOME_DIR/.local/bin"
ROUTER_PATH="$BIN_DIR/gpt-proxy-router.py"
SYSTEMD_DIR="$HOME_DIR/.config/systemd/user"
UNIT_PATH="$SYSTEMD_DIR/gpt-proxy-router.service"
BASHRC="$HOME_DIR/.bashrc"
CACHE_DIR="$HOME_DIR/.cache"
LOG_FILE="$CACHE_DIR/gpt-proxy-router.log"

MARK_START="# >>> gpt-proxy-router start >>>"
MARK_END="# <<< gpt-proxy-router end <<<"

log()   { printf '\033[1;32m[部署]\033[0m %s\n' "$1"; }
warn()  { printf '\033[1;33m[警告]\033[0m %s\n' "$1"; }
die()   { printf '\033[1;31m[失败]\033[0m %s\n' "$1"; exit 1; }

# ==================== 卸载模式 ====================
if [ "${1:-}" = "--uninstall" ]; then
    log "开始卸载..."
    systemctl --user disable --now gpt-proxy-router 2>/dev/null \
        && rm -f "$UNIT_PATH" \
        && systemctl --user daemon-reload 2>/dev/null \
        || true
    pkill -f "gpt-proxy-router\.py" 2>/dev/null || true
    rm -f "$ROUTER_PATH"
    rm -rf "$ROUTER_PATH".__pycache__ 2>/dev/null
    if [ -f "$BASHRC" ]; then
        sed -i "/^$MARK_START\$/,/^$MARK_END\$/d" "$BASHRC"
    fi
    log "卸载完成 (已删除路由程序/服务/环境变量块)"
    exit 0
fi

if [ -z "$UPSTREAM_HOST" ] || [ -z "$UPSTREAM_USER" ] || [ -z "$UPSTREAM_PASS" ]; then
    die "请先在脚本顶部配置区填写 UPSTREAM_HOST / UPSTREAM_USER / UPSTREAM_PASS"
fi

# ==================== 1. 环境检查 ====================
log "检查环境..."
command -v python3 >/dev/null 2>&1 || die "未找到 python3, 请先安装 Python 3.6+"
command -v curl    >/dev/null 2>&1 || warn "未找到 curl, 将跳过连通性验证"
PYTHON_BIN="$(command -v python3)"
PY_VER="$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
log "python3: $PYTHON_BIN (v$PY_VER)"

USE_SYSTEMD=0
if command -v systemctl >/dev/null 2>&1 && systemctl --user --quiet is-system-running 2>/dev/null; then
    USE_SYSTEMD=1
    log "检测到 systemd 用户服务 -> 使用开机自启方式"
else
    warn "systemd 用户服务不可用 -> 降级为 nohup + .bashrc 自启方式"
fi

# ==================== 2. 写入路由程序 ====================
log "写入路由程序 -> $ROUTER_PATH"
mkdir -p "$BIN_DIR" "$CACHE_DIR"
cat > "$ROUTER_PATH" <<'PYEOF'
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
本地智能路由代理 (GPT API 专用分流)

- OpenAI 相关域名  -> 上游 SOCKS5 代理
- 其他所有流量     -> 直连

配合环境变量 HTTP_PROXY/HTTPS_PROXY=http://127.0.0.1:17890 使用,
之后访问 api.openai.com 自动走代理, 无需任何特殊命令。

仅用 Python 标准库, 无第三方依赖。
"""

import logging
import select
import socket
import struct
import socketserver
import sys
from urllib.parse import urlsplit

LISTEN_HOST = "@LISTEN_HOST@"
LISTEN_PORT = @LISTEN_PORT@

UPSTREAM_HOST = "@UPSTREAM_HOST@"
UPSTREAM_PORT = @UPSTREAM_PORT@
UPSTREAM_USER = "@UPSTREAM_USER@"
UPSTREAM_PASS = "@UPSTREAM_PASS@"

# 需要走代理的域名 (后缀匹配)
PROXY_DOMAINS = (
    ".openai.com",
    ".oaistatic.com",
    ".oaiusercontent.com",
    ".chatgpt.com",
    ".openai.org",
    ".github.com",
    ".githubusercontent.com",
)

log = logging.getLogger("router")


def should_proxy(host: str) -> bool:
    host = (host or "").lower().rstrip(".")
    if not host:
        return False
    return any(host == d.lstrip(".") or host.endswith(d) for d in PROXY_DOMAINS)


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf += chunk
    return buf


def socks5_connect(host, port, timeout=25.0):
    """建立到上游 SOCKS5 的连接, 返回已连到目标主机的 socket."""
    s = socket.create_connection((UPSTREAM_HOST, UPSTREAM_PORT), timeout=timeout)
    s.settimeout(timeout)
    # 握手: 与curl一致提供3种方法 [no-auth, GSSAPI, user/pass]
    # (上游代理对2方法greeting会随机RST, 实测3方法100%成功)
    s.sendall(b"\x05\x03\x00\x01\x02")
    ver, method = recv_exact(s, 2)
    if ver != 0x05:
        raise RuntimeError("上游不是 SOCKS5 服务器")
    if method == 0x02:
        u = UPSTREAM_USER.encode("utf-8")
        p = UPSTREAM_PASS.encode("utf-8")
        s.sendall(b"\x01" + bytes([len(u)]) + u + bytes([len(p)]) + p)
        av, status = recv_exact(s, 2)
        if av != 0x01 or status != 0x00:
            raise RuntimeError("SOCKS5 认证失败")
    elif method != 0x00:
        raise RuntimeError(f"SOCKS5 方法被拒: {method}")
    # 连接请求 (域名形式, 由上游解析 DNS)
    hb = host.encode("idna")
    s.sendall(b"\x05\x01\x00\x03" + bytes([len(hb)]) + hb + struct.pack(">H", port))
    resp = recv_exact(s, 4)
    if resp[0] != 0x05 or resp[1] != 0x00:
        raise RuntimeError(f"SOCKS5 连接失败: {resp[1]}")
    atyp = resp[3]
    if atyp == 0x01:
        recv_exact(s, 4 + 2)
    elif atyp == 0x03:
        ln = recv_exact(s, 1)[0]
        recv_exact(s, ln + 2)
    elif atyp == 0x04:
        recv_exact(s, 16 + 2)
    return s


def relay(a, b):
    """双向转发, 同时监听可读与可写, 支持半关闭与大数据量流."""
    a.setblocking(False)
    b.setblocking(False)
    rlist = [a, b]
    wlist = []
    pending = {a: b"", b: b""}
    try:
        while True:
            r, w, _ = select.select(rlist, wlist, [], 600)
            for s in r:
                try:
                    data = s.recv(65536)
                except BlockingIOError:
                    continue
                except OSError:
                    data = b""
                if not data:
                    rlist.remove(s)
                    log.debug("relay: %s EOF", "客户端" if s is a else "上游")
                else:
                    other = b if s is a else a
                    pending[other] += data
                    if pending[other] and other not in wlist:
                        wlist.append(other)
            for s in w:
                data = pending[s]
                if data:
                    try:
                        n = s.send(data)
                    except BlockingIOError:
                        continue
                    except OSError as e:
                        log.warning("relay: 发往%s失败 %r", "上游" if s is b else "客户端", e)
                        return
                    if n > 0:
                        pending[s] = data[n:]
                        if not pending[s] and s in wlist:
                            wlist.remove(s)
                else:
                    if s in wlist:
                        wlist.remove(s)
            if not rlist and not wlist:
                return
    finally:
        for s in (a, b):
            try:
                s.close()
            except OSError:
                pass


class RouterHandler(socketserver.BaseRequestHandler):
    def _recv_line(self):
        data = b""
        while not data.endswith(b"\r\n"):
            try:
                chunk = self.request.recv(1)
            except OSError:
                return data
            if not chunk:
                return data
            data += chunk
            if len(data) > 65536:
                return data
        return data

    def _connect(self, host, port, timeout=25.0):
        if should_proxy(host):
            log.info("PROXY  %s:%s", host, port)
            return socks5_connect(host, port, timeout)
        log.info("DIRECT %s:%s", host, port)
        return socket.create_connection((host, port), timeout=timeout)

    def handle(self):
        try:
            self._handle()
        except Exception as e:
            log.warning("连接出错: %r", e)
        finally:
            try:
                self.request.close()
            except OSError:
                pass

    def _handle(self):
        first = self._recv_line()
        if not first:
            return
        parts = first.split()
        if len(parts) < 2:
            return
        method, target = parts[0], parts[1]

        if method == b"CONNECT":
            # HTTPS 隧道: host:port
            host_b, _, port_b = target.partition(b":")
            host = host_b.decode("ascii", "replace")
            try:
                port = int(port_b or b"443")
            except ValueError:
                port = 443
            # 消费 CONNECT 请求的剩余头部 (不能转发给目标服务器)
            while True:
                line = self._recv_line()
                if line in (b"\r\n", b""):
                    break
            upstream = self._connect(host, port)
            self.request.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
            relay(self.request, upstream)
            return

        # 普通 HTTP 请求 (absolute-form, 如 GET http://host/path)
        try:
            url = urlsplit(target.decode("ascii", "replace"))
            host = url.hostname or ""
            port = url.port or 80
            path = url.path or "/"
            if url.query:
                path += "?" + url.query
        except ValueError:
            return
        if not host:
            return

        upstream = self._connect(host, port)
        # 读取剩余请求头, 去掉 hop-by-hop 头
        headers = b""
        while True:
            line = self._recv_line()
            if line in (b"\r\n", b""):
                break
            low = line.lower()
            if low.startswith((b"connection:", b"proxy-connection:", b"proxy-")) \
                    or low.startswith(b"keep-alive:"):
                continue
            headers += line
        req = method + b" " + path.encode("ascii", "replace") + b" " + parts[2] \
            + b"\r\n" + headers + b"Connection: close\r\n\r\n"
        upstream.sendall(req)
        relay(upstream, self.request)


def main():
    logging.basicConfig(stream=sys.stderr, level=logging.INFO,
                        format="%(asctime)s %(message)s")

    class Server(socketserver.ThreadingTCPServer):
        allow_reuse_address = True
        daemon_threads = True

    server = Server((LISTEN_HOST, LISTEN_PORT), RouterHandler)
    log.info("智能路由代理已启动: %s:%s (OpenAI域名走代理, 其余直连)", LISTEN_HOST, LISTEN_PORT)
    server.serve_forever()


if __name__ == "__main__":
    main()
PYEOF

# 替换配置值 (用 | 作分隔符, 值中不含 | 即可)
sed -i \
    -e "s|@LISTEN_HOST@|$LISTEN_HOST|g" \
    -e "s|@LISTEN_PORT@|$LISTEN_PORT|g" \
    -e "s|@UPSTREAM_HOST@|$UPSTREAM_HOST|g" \
    -e "s|@UPSTREAM_PORT@|$UPSTREAM_PORT|g" \
    -e "s|@UPSTREAM_USER@|$UPSTREAM_USER|g" \
    -e "s|@UPSTREAM_PASS@|$UPSTREAM_PASS|g" \
    "$ROUTER_PATH" || die "配置替换失败"
chmod +x "$ROUTER_PATH"
python3 -m py_compile "$ROUTER_PATH" || die "路由程序语法检查失败"
rm -rf "$ROUTER_PATH".__pycache__

# ==================== 3. 停掉旧实例 ====================
if command -v pkill >/dev/null 2>&1; then
    pkill -f "gpt-proxy-router\.py" 2>/dev/null && log "已停止旧实例" || true
fi
sleep 0.3

# ==================== 4. 启动服务 ====================
if [ "$USE_SYSTEMD" = "1" ]; then
    log "创建 systemd 用户服务 -> $UNIT_PATH"
    mkdir -p "$SYSTEMD_DIR"
    cat > "$UNIT_PATH" <<EOF
[Unit]
Description=GPT API smart proxy router (OpenAI domains via SOCKS5, others direct)
After=network-online.target

[Service]
ExecStart=$PYTHON_BIN $ROUTER_PATH
Restart=on-failure
RestartSec=3

[Install]
WantedBy=default.target
EOF
    systemctl --user daemon-reload || die "daemon-reload 失败"
    systemctl --user enable --now gpt-proxy-router || die "服务启动失败, 查看: journalctl --user -u gpt-proxy-router -n 30"
    sleep 1
    systemctl --user is-active --quiet gpt-proxy-router || die "服务未运行"
    log "服务已启动并设为开机自启"
else
    log "以 nohup 后台启动..."
    nohup python3 "$ROUTER_PATH" >> "$LOG_FILE" 2>&1 &
    sleep 1
    if ! kill -0 $! 2>/dev/null; then
        die "启动失败, 查看日志: $LOG_FILE"
    fi
    log "已后台启动 (PID $!)"
fi

# ==================== 5. 写入 .bashrc (幂等) ====================
log "配置 ~/.bashrc 环境变量..."
if [ -f "$BASHRC" ]; then
    sed -i "/^$MARK_START\$/,/^$MARK_END\$/d" "$BASHRC"
fi
{
    echo "$MARK_START"
    echo "export HTTP_PROXY=\"http://$LISTEN_HOST:$LISTEN_PORT\""
    echo "export HTTPS_PROXY=\"http://$LISTEN_HOST:$LISTEN_PORT\""
    echo "export http_proxy=\"\$HTTP_PROXY\""
    echo "export https_proxy=\"\$HTTPS_PROXY\""
    echo "export NO_PROXY=\"localhost,127.0.0.1,::1\""
    echo "export no_proxy=\"\$NO_PROXY\""
    if [ "$USE_SYSTEMD" = "0" ] && command -v pgrep >/dev/null 2>&1; then
        echo "pgrep -f \"python3 .*gpt-proxy-router.py\" >/dev/null 2>&1 || nohup python3 \"\$HOME/.local/bin/gpt-proxy-router.py\" >> \"\$HOME/.cache/gpt-proxy-router.log\" 2>&1 &"
    fi
    echo "$MARK_END"
} >> "$BASHRC"
log "环境变量已写入 (幂等块: $MARK_START ... $MARK_END)"

# ==================== 6. 验证 ====================
if command -v curl >/dev/null 2>&1; then
    log "验证 OpenAI 连通性 (期望 401 = 网络已通)..."
    CODE="$(curl -s -o /dev/null -w "%{http_code}" -x "http://$LISTEN_HOST:$LISTEN_PORT" \
        --connect-timeout 20 https://api.openai.com/v1/models 2>/dev/null || echo 000)"
    if [ "$CODE" = "401" ]; then
        log "验证通过: api.openai.com 已通过代理连通 (HTTP 401)"
    else
        warn "验证未通过 (HTTP $CODE)。请检查:"
        warn "  1. 上游代理账号/密码/地址是否正确 (脚本顶部配置区)"
        warn "  2. 本机到 $UPSTREAM_HOST:$UPSTREAM_PORT 的网络是否可达"
        warn "  3. 日志: journalctl --user -u gpt-proxy-router -n 30 或 $LOG_FILE"
    fi
fi

# ==================== 7. 完成提示 ====================
cat <<DONE

============================================
  部署完成!
============================================
下一步:
  1. 重新打开终端 (或执行: source ~/.bashrc)
  2. 重启需要联网的应用 (opencode / 编辑器等)
  3. 之后访问 OpenAI 自动走代理, 其他网站直连

常用命令:
  查看分流日志:  journalctl --user -u gpt-proxy-router -f
  手动测连通性:  curl https://api.openai.com/v1/models
  卸载:          bash $0 --uninstall

新增需要走代理的域名: 编辑 $ROUTER_PATH 中的 PROXY_DOMAINS
============================================
DONE
