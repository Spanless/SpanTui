# 安全边界

## 1. 文件路径

- `FileTreeModel`、`EditorController`、`DocumentManager` 操作的路径先规范化（`QDir::cleanPath`/`canonicalFilePath`），拒绝 `..` 段，且必须位于当前 `projectRoot` 内。
- 创建或重命名时拒绝 Windows 保留设备名（`CON`、`PRN`、`AUX`、`NUL`、`COM1..9`、`LPT1..9`）及其变体。
- 批量替换跳过符号链接与重解析点。

## 2. 外部进程

- 子进程参数一律用 `QStringList` 传递，不拼接 `cmd /c` 字符串。
- SSH：主机名、用户、端口、私钥路径先经 `checkEndpoint()` 校验（拒绝 `-` 开头及空白、引号、`%` 等），以独立 argv 传递并在主机名前加 `--`；远程 shell 路径经 `shellQuote()`，sftp 操作数经 `sftpQuote()`（见 [SSH 远程主机](../services/remote/ssh-remote.md)）。
- 文件树运行脚本只接受本地绝对路径，扩展名限 `.bat`、`.cmd`、`.ps1`。

## 3. Markdown 远程内容

Markdown 以 GFM + `MarkdownNoHTML` 装载；raw HTML 经有界适配器转换，主动标签、事件属性、style 与危险 URL 不进入文档。远程图片由 `MarkdownRemoteImageLoader` 加载：

- `isPublicAddress` 拒绝 DNS 解析结果中的 `127.0.0.0/8`、`10.0.0.0/8`、`100.64.0.0/10`、`172.16.0.0/12`、`192.168.0.0/16`、`198.18.0.0/15`、`169.254.0.0/16`、`fc00::/7`、`fe80::/10`、`::1`。
- 每次重定向重新校验 URL、DNS 与地址，HTTPS 不降级；连接固定到已校验 IP，保留原 Host/SNI。
- 预算：单图 10 MiB，解码与缓存 64 MiB，单边 8192 像素、总 1600 万像素，并发 3，重定向 3。

## 4. 本地管道

- ConPTY 管道（`\\.\pipe\SpanTui-<pid>-<counter>-<direction>`）由 `OwnerOnlyPipeSecurity` 设置只含当前用户 SID 的 DACL。
- `SshAskpassBroker` 管道用 `QLocalServer::UserAccessOption`，只回答带本进程签发且未撤销令牌的请求。

## 5. 日志

诊断、心跳与崩溃日志（`hang-latest.log`、`crash.log`）只记录枚举、代次与计数；不写终端输出、键盘输入、用户代码或凭据。

## 6. 凭据

- 密码与私钥密码以 DPAPI（当前用户作用域 + 固定熵，`src/platform/windows/credential_protection.*`）密文存入 `remote_hosts` 表。明文只在进程内存与 askpass 一次往返中，不进日志、消息中心或 QML（对话框只知"是否已保存"）。
- 私钥导入为 `<root>\ssh-keys\<hostId>`，创建时即带只含当前用户的受保护 DACL（满足 OpenSSH 权限检查）；暂存 `<hostId>.staged` 与备份 `<hostId>.previous` 在同目录改名，保留该 DACL。用户原文件不改。
