# SSH 远程主机

远程主机提供终端、文件浏览与资源读数，不提供 Git。

## 1. 结构

不内置 SSH 协议，每条连接是一个 OpenSSH 进程。可执行文件由 `ToolRegistry` 决定：用户指定路径 > `%SystemRoot%\System32\OpenSSH` > PATH。ssh 不可用时隐藏侧栏「远程」节（记录保留）；只缺 sftp 时上传下载报错。

| 用途 | 进程 | 所有者 |
|---|---|---|
| 远程终端 | `ssh [选项] -p <端口> [-l <用户>] -- <主机>`，在 ConPTY 中 | `TerminalSession` |
| 文件操作、资源读数 | 常驻 `ssh -T … -- <主机> "exec /bin/sh"`，经管道发脚本 | `RemoteShellChannel`（文件浏览与 `RemoteSystemMonitor` 共用） |
| 上传下载 | 每批一个 `sftp -o BatchMode=no … -b - …` | `RemoteTransferQueue` |
| 口令、私钥密码、主机密钥确认 | ssh 以本程序为 `SSH_ASKPASS` | `SshAskpassBroker` + `RemoteHostRegistry` |

`src/remote/`（`SpanRemote`，依赖 `SpanCore`，凭据加密另用 `SpanPlatform`）：命令构造、askpass、shell 通道、文件模型、传输队列，以及主机目录 `RemoteHostRegistry` 与每主机上下文 `RemoteHostContext`。作用域键见 `src/workspace/scope.h`。

## 2. 主机记录与作用域

- `RemoteHostRegistry`，表 `remote_hosts`；QML 单例 `RemoteHosts`。
- 用户名留空即 `root`（否则 ssh 用本机 Windows 用户名）。
- `checkEndpoint()` 拒绝以 `-` 开头或含空白、引号、`%` 等的主机名与用户名；端口 1–65535；私钥须为已存在文件。所有值作为独立 argv 传递，不拼进 shell 字符串。
- 远程会话与其瓦片的 `projectPath` 存 `remote:<hostId>`（由 `span::workspace::Scope` 解析与生成，`src/workspace/scope.h`），`SessionModel` 不对其做路径规范化，本地 cwd 为用户主目录。启动时仅在主机列表读取成功后删除主机已不存在的远程会话；读取失败时全部保留但不启动。布局对账把远程作用域视为有效。
- 删除主机（`WorkspaceController::removeRemoteHost`）：先删记录、凭据、私钥副本与上下文，成功后才结束会话。

## 3. 凭据与提示桥

1. **加密**：密码、私钥密码以 DPAPI（当前用户 + 固定熵）加密存 `remote_hosts.password`/`key_passphrase`（`src/platform/windows/credential_protection.*`）。明文不返回 QML，`hostDetails()` 只给 `hasPassword`/`hasKeyPassphrase`。
2. **askpass**：ssh/sftp 进程带 `SSH_ASKPASS=<本程序>`、`SSH_ASKPASS_REQUIRE=force`、管道名与按主机签发的令牌。`main()` 首行识别该调用，经仅当前用户可访问的管道把提示交给 broker，答复写 stdout 后退出，不启动 GUI。未知令牌一律拒绝。
3. **自动作答**（`RemoteHostRegistry::handlePrompt`）——保存的秘密只回答确认属于本主机登录的提示：
   - **进程归属**（`isLaunchedSsh`）：发问的 ssh 须由本程序直接启动，或是本程序启动的 sftp 的子进程。ProxyJump 跳板继承了 askpass 环境但低一层，必须排除；提示文字本身不含端口，无法区分。
   - **提示种类**（`classifySshPrompt`，按 OpenSSH 原文核对）：密码只认 `user@host's password: ` 与 keyboard-interactive 的单纯 `Password:`；私钥口令只认 `Enter passphrase for key '<路径>'` 且路径是本主机副本（ssh 截断到 100 字符，按前缀比对）；其他提到 password/passphrase 的归为一次性 `Secret`。主机密钥只认 ssh 自己的 `(yes/no` 提问。
   - 不满足条件的秘密提示照常弹框，但不自动填、不提供记住、不缓存、不转答其他提示。
   - 同进程同类提示第 1 次用已输入或已保存的值作答；第 2 次起（说明被拒）清缓存并弹 `SshPromptDialog` 提示上次未通过。
   - 本次运行已信任的同一主机密钥直接答 `yes`；终端与文件通道同时连接时只问一次。
4. **记住**：提示框「记住」默认开，答案以 DPAPI 写回。
5. **私钥副本**：Windows OpenSSH 拒绝其他账户可读的私钥，而数据盘文件常继承宽松 ACL。保存主机时复制到数据库同目录 `ssh-keys\<hostId>`（文件与目录只含当前用户一条 ACE，`writeOwnerOnlyFile`/`makeOwnerOnlyDirectory`），ssh/sftp 只用副本，原文件不动。
   - 每次保存重新导入；清空私钥字段即删副本；原文件被移走而路径未改时继续用副本；启动时为缺副本且原文件在的主机补导入。
   - **与记录同成同败**：新副本写到 `<hostId>.staged`；数据库事务内写行、现有副本改名 `.previous`、暂存原子改名为正式（`replaceFile`）；提交后才删 `.previous`。任一步失败回滚事务、删暂存、恢复 `.previous`。
   - 删不掉的副本（被占用）告警，下次启动 `removeOrphanKeyCopies()` 清理不属于任何主机的文件（含 `.staged`/`.previous`）。
   - 副本是明文私钥，保护靠 ACL。
6. **改目标即换令牌**：保存成功且改了地址、端口或用户时（`retireSession`，删除主机同样调用），吊销旧令牌，丢弃为旧服务器输入的密码、信任的主机密钥与待答提示。仍连旧服务器的进程再提问会被拒绝，拿不到新服务器的密码。未重填密码时清除已保存密码；私钥密码随私钥保留。只改名称或私钥路径不换令牌。

askpass 管道无法监听时（告警），ssh 用自己的控制台提示：远程终端可用，文件浏览只能用密钥或 agent。状态经 `promptBridgeAvailable`/`promptBridgeError`/`promptBridgePipe` 显示在设置远程页；`RemoteFileModel` 在此时连接失败会补充说明。

## 4. 远程终端

`WorkspaceController::createRemoteSession(hostId)` 新建会话。`connectSessionTerminals` 为远程作用域注入 `commandLine`（MSVC 引号规则）与 `environment`（askpass 变量 + `TERM=xterm-256color`，服务器通常没有本地终端的 terminfo）。`hostsChanged` 时 `TerminalManager::synchronize` 更新会话的启动参数，运行中进程不受影响，重连即用新端点。

## 5. 远程文件浏览

聚焦远程终端时 `boundRemoteHostId` 非空，右栏只显示 `RemoteFilePane`。

- **模型**：`RemoteFileModel`，每主机一个，首次显示时连接并进入主目录。文件夹在前，可按名称/大小/时间排序、切换隐藏文件。列表请求带代次，旧结果丢弃。只有退出码 0 且带 NUL 结尾记录才算列表；2（进不去）、3（无 GNU find，先用 `find . -maxdepth 0 -printf` 探测）、4（不可读）报错并保留当前目录。地址栏绑定 `currentPath`，编辑结束后重新绑定。
- 改了地址、端口、用户或私钥（由注册表以 `keyChanged` 告知）时 `configure()` 结束旧通道、清空列表，在新主机主目录重连；只改名称不影响连接。
- **shell 通道**：每个请求包成 `printf '\001<nonce>:<id>:B'` … `( 脚本 ) </dev/null` … `printf '\001<nonce>:<id>:E:%d' "$?"`，nonce 每连接随机，登录横幅不会被当成帧。路径单引号转义。单帧上限 256 MB，超出断开并报错。
- **操作**：新建、重命名（目标存在则拒绝）、删除（`rm -rf --`，只接受规范化绝对路径且非 `/`，先确认）、复制路径、上传（工具栏或拖放）、下载。
- **传输**：`RemoteTransferQueue` 逐批运行 sftp，任务入队时固定主机参数。批处理操作数写成双引号字符串，只转义 `"` 与 `\`（sftp 不对引号内 `* ? [` 通配，多加反斜杠会成为文件名一部分）。IPv6 主机加方括号（sftp 按 `[user@]host[:path]` 解析）。进度归 `Remote`，失败进消息中心，上传到当前目录时刷新。sftp 在管道上全缓冲输出，故不做交互式 sftp 通道。

## 6. 服务器资源读数

远程终端标题栏显示服务器读数而非本机 ssh.exe（界面见 [资源监控](../monitoring/resource-monitoring.md)）。

- `RemoteSystemMonitor`，每主机一个；QML 经 `WorkspaceController.remoteContextForScope(<作用域>).systemMonitor` 取得。
- 经共用的 `RemoteShellChannel` 运行 `remoteSystemSampleScript()`：`/proc/stat`（必需）、`/proc/meminfo`（缺 `MemAvailable` 时用 MemFree + Buffers + Cached）、`/proc/net/dev`、`df -Pk /`、`/proc/loadavg`。CPU 与网速取相邻读数之差；回环、`docker*`、`br-*`、`virbr*`、`veth*` 不计入。
- 按可见消费者激活（`setConsumerVisible`），间隔 3 秒，最多一个请求在途；停用时丢弃在途请求的回复（隐藏后不再更新读数），重新激活时丢弃旧基线。
- 通道 Failed 时不再提交（否则每次都重启 ssh 并弹密码框），用户重连后恢复。非 Linux（退出码 3 或无 CPU 行）时对该连接停止采样。

## 7. Tailscale 设备

设置远程页列出 tailnet 设备以便添加主机（界面见 [远程页](../application/settings-appearance.md#远程页)）。添加结果是普通主机记录，地址为 Tailscale IPv4，不存任何 Tailscale 专属字段。

- `TailscaleDeviceModel`（`src/remote/tailscale_devices.*`，QML 单例 `TailscaleDevices`）；CLI 为可选工具 `ExternalTool::Tailscale`，不可用时 `state` 为 `unavailable`。
- `refresh()` 时运行一次 `tailscale status --json`（8 秒超时，同时一个），不轮询；工具路径变化且已读过时重读。任意退出码下输出是状态 JSON 即采用；否则 `error`，取 stderr（或 stdout）首个非空行。`BackendState` 映射为 `running`/`needsLogin`（含 NeedsMachineAuth）/`starting`/`stopped`（含 NoState——Windows 上服务未被客户端连起时一直停在 NoState）。非 `running` 不列设备；带 `Location` 的对端（出口节点）不列。
- **SSH 判定**：发布了 `sshHostKeys` 即 Tailscale SSH；其余在线设备对首个地址（IPv4 优先）的 22 端口做一次 TCP 连接，3 秒内连上为可连；离线不探测。每次刷新递增代次，旧探测丢弃。
- `sshOnly`（默认开，不持久化）只留可 SSH 设备。顺序：在线优先，再按机器名（MagicDNS 名首段，无则系统主机名）。
- `RemoteHostRegistry::hostIdForAddress([地址, MagicDNS 名])` 命中时该行改为「连接」。

## 8. 代码与测试

- `src/remote/{ssh_command, ssh_askpass, remote_shell_channel, remote_file_listing, remote_file_model, remote_transfer_queue, remote_system_monitor, tailscale_devices}.*`
- `src/remote/{remote_host_registry, remote_host_context}.*`、`src/workspace/scope.h`；QML：`qml/components/business/remote/`
- 测试：`ssh_command_test`、`ssh_askpass_test`、`remote_shell_channel_test`、`remote_system_monitor_test`、`tailscale_devices_test`、`remote_host_registry_test`、`session_model_test`、`workspace_controller_test`。依赖 Git for Windows 的 `sh.exe`/`sftp-server.exe` 的用例在缺失时跳过。没有端到端 SSH 服务器测试，与真实服务器的交互需人工验证。

## 9. 本域改动完成标准

- [ ] ssh/sftp 参数以独立 argv 传递；用户输入先过 `checkEndpoint()`，远端路径经 `shellQuote()`/`sftpQuote()`。
- [ ] 凭据只以 DPAPI 密文落盘，不进日志、消息中心或 QML；私钥只以仅当前用户可访问的副本存在于 `ssh-keys\`。
- [ ] `dev.bat test -R ssh_ -R remote_ -R session_model -R workspace_controller -R migration -R translation_catalog`。
