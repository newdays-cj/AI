# GPT 智能路由代理 — 部署与工作流程记录

> 目标：在国内网络环境下，让所有程序（curl / Python / opencode / git 等）**零特殊命令**访问 GPT API，
> 同时不影响其他网站直连。本文档记录完整部署方式、调试过程和踩坑经验，供后续参考。

---

## 1. 背景与目标

| 问题 | 现状 |
|---|---|
| `api.openai.com` 直连 | ❌ 超时（被墙，且 DNS 污染） |
| `ping api.openai.com` | ❌ 不通（正常现象，见 FAQ-1） |
| 通过日本 SOCKS5 代理 | ✅ 可达 |

目标：**按域名自动分流** —— OpenAI 相关域名自动走代理，其他流量直连，无需任何特殊命令。

## 2. 总体架构

```
任何程序 (curl / python / opencode / git ...)
    ↓ 环境变量 HTTP_PROXY/HTTPS_PROXY=http://127.0.0.1:17890
本地智能路由 (gpt-proxy-router, systemd 开机自启)
    ├─ *.openai.com / chatgpt.com / github.com 等 → 日本 SOCKS5 代理
    └─ 其他所有流量 → 直连
```

## 3. 文件组成

| 文件 | 说明 |
|---|---|
| `deploy-gpt-proxy.sh` | 一键部署脚本（内嵌路由程序，含卸载模式） |
| `~/.local/bin/gpt-proxy-router.py` | 本地路由程序（部署后生成，纯 Python 标准库） |
| `~/.config/systemd/user/gpt-proxy-router.service` | systemd 用户服务（无 systemd 时自动降级为 nohup） |
| `~/.bashrc`（幂等块） | 代理环境变量（带标记，重复部署不产生重复配置） |

## 4. 一键部署

```bash
# 1. 编辑配置区（必填 3 项）
vim deploy-gpt-proxy.sh
#   UPSTREAM_HOST / UPSTREAM_USER / UPSTREAM_PASS

# 2. 部署
bash deploy-gpt-proxy.sh

# 3. 生效
source ~/.bashrc      # 或重开终端
# 重启需要联网的应用 (opencode / 编辑器等)

# 卸载
bash deploy-gpt-proxy.sh --uninstall
```

部署脚本自动完成：环境检查 → 写入路由程序 → 停旧实例 → 启动服务 → 幂等写 .bashrc → 自动验证（期望 401）。

## 5. 验证方法

```bash
# 网络连通性（401 = 已到达 OpenAI 服务器，只差认证）
curl https://api.openai.com/v1/models

# 查看分流日志（确认哪些域名走了 PROXY / DIRECT）
journalctl --user -u gpt-proxy-router -f
```

## 6. 调试记录 — 三个关键 bug（重要参考）

### Bug 1: CONNECT 剩余请求头未消费 → TLS 流被污染
- **现象**：所有通过路由的 HTTPS 连接全部失败
- **原因**：收到 `CONNECT host:443 HTTP/1.1` 首行后未读取剩余请求头，`relay` 把残留的
  `Host:`/`User-Agent:` 等头转发给了目标服务器，污染 TLS 握手
- **修复**：发 `200 Connection Established` 前，先循环读到空行消费完剩余头

### Bug 2: SOCKS5 greeting 只提供 2 种认证方法 → 上游随机 RST（约 50% 失败）
- **现象**：连接时好时坏，成功率约 50%，RST 发生在 TLS ClientHello 之后
- **排查**：自制伪 SOCKS5 服务器逐字节抓包对比 curl 的实现，发现 curl 的 greeting 是
  `05 03 00 01 02`（3 种方法：no-auth/GSSAPI/user-pass），我的实现是 `05 02 00 02`（2 种）
- **验证**：A/B 对照测试（交错执行排除时间因素）——2 方法 3/6 失败，3 方法 6/6 全成功
- **修复**：greeting 改为与 curl 完全一致的 `\x05\x03\x00\x01\x02`

### Bug 3: relay 只监听可读、不监听可写 → 大数据量断连
- **现象**：opencode 报 "socket connection was closed unexpectedly"，curl 小请求却正常
- **原因**：非阻塞 socket 上 `sendall`，当上游发送缓冲区写满时立即抛 `BlockingIOError`，
  被当 OSError 捕获后直接关闭连接。opencode 请求体几十 KB，小请求测试暴露不了
- **修复**：重写 relay 为可读+可写双向 `select`，带 pending 缓冲区，支持半关闭

**调试方法论**：抓包对比（伪服务器记录字节）→ 控制变量 → A/B 交错对照 → 最小修复。

## 7. 常见问题排查 (FAQ)

1. **`ping api.openai.com` 不通是正常的** — ping 走 ICMP，代理只转发 HTTP/HTTPS，
   判断连通性请用 `curl https://api.openai.com/v1/models`（401 即通）
2. **改了 .bashrc 应用还是连不上** — 环境变量只在进程启动时读取，必须**完全重启**应用
3. **git push 报 "Proxy CONNECT aborted"** — 路由器对该域名走了直连但直连超时（如 github），
   把域名加入 `gpt-proxy-router.py` 的 `PROXY_DOMAINS` 后 `systemctl --user restart gpt-proxy-router`
4. **返回 401** — 网络已通，需配置认证：API Key（`Authorization: Bearer sk-...`）
   或 OAuth 登录（opencode 用 `opencode auth login`）
5. **上游代理随机失败** — 检查代理服务商是否限速/限制并发；本方案已按 curl 行为优化握手

## 8. 配置修改指南

| 需求 | 操作 |
|---|---|
| 换代理账号 | 编辑 `deploy-gpt-proxy.sh` 配置区，重新部署 |
| 新域名走代理 | 编辑 `gpt-proxy-router.py` 的 `PROXY_DOMAINS`，重启服务 |
| 换本地端口 | 脚本配置区 `LISTEN_PORT`（同时需改 .bashrc 中的端口） |
| 查看实时分流 | `journalctl --user -u gpt-proxy-router -f` |

## 9. 关键约束

- 路由程序仅用 Python 标准库，无第三方依赖，兼容 Python 3.6+
- 认证信息用后即删，**切勿把代理账号密码提交到 git 仓库**
- 部署脚本内嵌的路由代码与 `~/.local/bin/gpt-proxy-router.py` 保持同步（改动一处需同步另一处）
