# Markdown 预览

CommonMark 0.31.2 加 Qt 支持的 GFM 0.29 表格、任务列表、删除线与扩展自动链接；不复制 GitHub.com 的私有后处理。Markdown 文件只走只读预览，由 `MarkdownDocumentRenderer` 向 `QTextDocument` 单次写入。

## 1. 解析与安全

原文不经正则重写，由 `QTextDocument::setMarkdown(MarkdownDialectGitHub | MarkdownNoHTML)` 解析。`MarkdownGfmAdapter` 只处理有界的 raw HTML 片段与 GitHub alert（产品扩展，非 GFM 规范）：

- 代码块与行内代码逐字保留。
- `br`、`code`、`a`、`img`/`picture`、标题与有限的 center 容器转为安全 Markdown 或展示元数据。
- script、iframe、object、embed、事件属性、style、危险 URL 不进入文档；无法确定安全的 HTML 降级为文本，不扩大白名单。

### 链接（`MarkdownPreview.onLinkActivated`）

- `#anchor` 文档内跳转；只有 `http(s)://` 交给系统浏览器；其他 scheme（含盘符路径）忽略。
- 相对链接按文档目录解析，经 `fileLinkActivated` → `WorkspaceController::openFileUrl` 在查看器中打开，目标须是项目内已存在的文件。本地链接从不交给 `Qt.openUrlExternally`，链接到 `.bat`/`.exe` 只显示文本。

## 2. 远程图片

- 所有者：`MarkdownRemoteImageLoader`（请求、DNS pin、重定向、generation、队列、LRU）；渲染器收集 URL 集、持有许可并回填；`MarkdownNetworkPolicy` 是纯 URL/IP 判断。
- **默认不加载**（远程图片可作追踪像素）。渲染器只统计去重后的 `remoteImageCount`；`PreviewPane` 显示提示条，点击 `allowRemoteImages()` 后本文档 URL 才进入 loader。许可只在渲染器内存，任何 `source` 变化（切换、重开、外部修改重载）都撤回；切换主题或宽度不撤回。
- 放行后策略不变：非公网地址（内网、回环、链路本地）仍拒绝；初始请求与每次重定向都重新验证 scheme、userinfo、host 与全部解析地址，HTTPS 不降级。`setActive(false)` 与 `source` 变化取消在途请求。
- 上限：单图下载 10 MiB；解码 64 MiB、单边 8192、1600 万像素；缓存 64 MiB；并发 3；重定向 3。当前文档图片被 LRU 淘汰时同步清除文档资源（不计入失败）；失败的 URL 在重试间隔内不再请求。

## 3. 代码与测试

- `src/documents/markdown_{document_renderer, gfm_adapter, network_policy, remote_image_loader}.*`、`qml/components/business/preview/MarkdownPreview.qml`
- `tests/markdown/`：`markdown_document_renderer_test`、`markdown_network_policy_test`、`markdown_remote_image_loader_test`（本机 HTTP 夹具，经 `setNetworkPolicyForTesting` 放行回环）；`workspace_controller_test`（`openFileUrl`）
