# C++ 大文件上传下载系统

这是一个基于 HTTP 的文件上传/下载系统，后端使用 C++，前端使用 HTML 页面，支持单文件大小到 200G 及以上（依赖磁盘和文件系统能力）。文件固定保存到服务启动目录下的 `upload/`。

## 特性

- 支持大文件：使用 64 位文件大小（`uint64_t`）进行传输
- 不限速传输：代码中无任何限速逻辑，按网络与磁盘上限传输
- 流式分块：8 MiB 缓冲区，避免整文件加载到内存
- 并发处理：服务端每个连接独立线程
- HTML 前端：浏览器直接上传/下载
- 上传进度显示：前端实时展示上传百分比
- 上传详情显示：实时展示已上传大小/总大小、上传速度、预计剩余时间
- 自动文件列表：前端自动展示 `upload/` 中所有文件并一键下载
- 固定上传目录：无需手动指定服务器保存路径（为当前启动目录下的 `upload/`）
- 基础安全：文件名做了简单清洗

## 目录结构

- `src/server.cpp`：HTTP 服务端
- `src/common.cpp/.hpp`：通用传输与路径工具
- `web/index.html`：前端页面

## 构建

```bash
cmake -S . -B build
cmake --build build -j
```

生成可执行文件：

- `build/file_server`

## 运行

1. 启动服务端

```bash
./build/file_server 9000
```

2. 浏览器打开

```bash
http://127.0.0.1:9000/
```

3. 在页面执行上传/下载

- 上传：选择本地文件后直接上传，页面显示实时进度
- 下载：页面自动显示所有文件，点击对应下载按钮即可

## HTTP API

### 上传

- `POST /api/upload`
- 请求头：
  - `X-File-Name: <file_name>`
  - `Content-Length: <size>`
- 请求体：文件二进制流（raw body）

成功返回：`200 OK`

### 下载

- `GET /api/download?name=<file_name>`

成功返回：

- `200 OK`
- `Content-Type: application/octet-stream`
- `Content-Length: <size>`
- 响应体为文件二进制流

### 文件列表

- `GET /api/files`
- 返回 JSON，例如：`{"files":["a.bin","b.iso"]}`

### 页面

- `GET /`
- 返回 `web/index.html`

## 命令行测试示例

如果你想不用页面，也可以用 curl 直接测试：

```bash
curl -X POST \
  -H "X-File-Name: big.bin" \
  --data-binary @./big.bin \
  http://127.0.0.1:9000/api/upload

curl -L "http://127.0.0.1:9000/api/download?name=big.bin" -o ./downloaded.bin

curl -L "http://127.0.0.1:9000/api/files"
```

## 注意事项

- 当前实现是一个精简版本，未包含断点续传、鉴权、TLS、校验和。
- 若用于生产环境，建议增加：
  - 鉴权与访问控制
  - TLS 加密
  - 断点续传（按 offset 上传/下载）
  - 完整性校验（例如 SHA-256）
  - 连接与线程模型优化（线程池/epoll/kqueue）
