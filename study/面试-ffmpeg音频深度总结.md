# FFmpeg 音频处理深度总结（面试准备）

> 本文基于 FFmpeg 上游 commit `554dcc2885` 的源码阅读，以及 `study/audio_flow/` 实战踩坑经验整理。
> 目标：能就 FFmpeg 音频处理全链路在技术面试中讲清楚"为什么这么设计"而不只是"怎么用"。

**文档结构**
1. 音频处理全链路总览
2. 分层源码深度剖析（libavformat → libavcodec → libavutil → libswresample）
3. 50 道面试问答（按主题分组）
4. 5 个"使用 FFmpeg 的坑"（STAR 格式）
5. 5 个 FFmpeg 源码层面的真实 bug

---

## 1. 音频处理全链路总览

```
网络 URL (mp3)
    │
    ▼ [libavformat] avformat_open_input + avformat_find_stream_info
AVFormatContext (包含 HTTP protocol + mp3 demuxer)
    │
    ▼ av_read_frame
AVPacket (压缩数据 + pts/dts)
    │
    ▼ [libavcodec] avcodec_send_packet → avcodec_receive_frame
AVFrame (PCM, fltp planar float, 44100Hz, stereo)
    │
    ▼ [libswresample] swr_convert
PCM (s16 interleaved, 44100Hz, stereo)
    │
    ▼ [应用层 + SDL/PortAudio/CoreAudio]
扬声器 🔊
```

**每层一句话概括：**

| 层             | 负责            | 核心抽象                                                      |
| ------------- | ------------- | --------------------------------------------------------- |
| libavformat   | I/O 协议 + 解复用  | `AVFormatContext`, `AVIOContext`, `AVStream`, `AVPacket`  |
| libavcodec    | 解码            | `AVCodecContext`, `AVFrame`, push-style send/receive API  |
| libavutil     | 公共基础设施        | `AVChannelLayout`, `AVBufferRef`（引用计数）, `AVRational`（时间基） |
| libswresample | 重采样/格式转换/声道混合 | `SwrContext`                                              |

**"I/O 协议 + 解复用" 是两个不同层次，libavformat 把它们都包了（按数据流方向，协议在前、解复用在后）：**

- **I/O 协议（Protocol）—— 字节从哪来。** 负责把字节搬到内存，不关心字节内容。`file://` 走 `file.c`（read/write 系统调用），`http://` 走 `http.c`（TCP + HTTP 请求），还有 `rtmp`、`tcp`、`pipe` 等等。抽象出口是 `AVIOContext`——一个会吐字节的管道。
- **解复用（Demuxer）—— 字节里装了什么。** 拿到一串字节后，按容器格式拆出音频流、视频流、字幕流。MP3 文件交给 `mp3dec.c`，MP4 交给 `mov.c`，Matroska 交给 `matroskadec.c`。输出是一个个 `AVPacket`（带时间戳的压缩数据块）。

```
URL → [I/O 协议] → 字节流 → [解复用] → AVPacket → [解码器] → AVFrame
       file/http             mp3/mp4             mp3/aac
```

`avformat_open_input(&fmt_ctx, "https://x.com/a.mp3", ...)` 这一行做了两件事：协议层看到 `https://` 用 http 打开拿到字节流，解复用层探测字节头认出是 MP3 用 mp3 demuxer 解析。两层完全解耦——**同一个 mp3 demuxer 既能处理本地文件也能处理 HTTP 流**，因为它只面对 `AVIOContext` 这个抽象管道。这就是为什么 `audio_flow/main.c` 里本地文件和网络 URL 走完全同一条代码路径。

**一个关键设计：纯 demux/decode 分离**

FFmpeg 把解复用和解码彻底隔开，中间用 `AVPacket` 传递。好处：
- 一个 demuxer（如 mp3dec）可接任意传输层（http/file/pipe）
- 一个 decoder（如 mp3float）可被任意 demuxer 喂数据
- Remuxing（改封装不改编码）和 Transcoding（改编码）是同一套 API

---

## 2. 源码深度剖析（按全链路走一遍）

本节沿着 §1 的流水线图，**逐个函数钻进去看"里面到底在做什么"**。顺序就是数据流动的顺序：

```
avformat_open_input  →  find_stream_info  →  av_read_frame
       ↓                                           ↓
   AVIOContext                                  AVPacket
                                                   ↓
  avcodec_send_packet  →  avcodec_receive_frame  →  MP3 decoder internals
                                                   ↓
                                                 AVFrame (fltp)
                                                   ↓
                                           swr_convert  →  SDL_QueueAudio
```

最后再单独讲 §2.10 横切基础（AVChannelLayout / AVBufferRef / 时间基 / 线程），这些是穿插在每一段里出现的共用机制，统一成一节讲更清楚。

---

### 2.1 `avformat_open_input` — 从 URL 到 AVIOContext + 首帧定位

对应 `libavformat/demux.c:231`。看着 5 个参数很简单，内部干了 **6 件事**，沿着调用链一层一层展开：

1. **URL → 协议匹配**：从 `"https://..."` 抠出 scheme，在协议表里找到 `ff_http_protocol`，分配 `URLContext`。
2. **HTTP 连接建立**：`http → tls → tcp` 逐层递归打开，发 `GET` + `Range: bytes=0-`，解析响应头拿到 `filesize` / `is_streamed`。
3. **URLContext → AVIOContext 封装**：`ffio_fdopen` 套一层读写缓冲，暴露给上层 demuxer 的是 `AVIOContext`（带 `read_packet`/`seek` 回调）。
4. **格式探测**：`av_probe_input_buffer2` 指数增长读 2K→1MB，每轮调 `av_probe_input_format3` 让所有 demuxer 打分，选最高分 + MIME 加成。
5. **MP3 probe 的连续帧校验**：`mp3_read_probe` 扫头找合法 MPEG sync，用 `header_emu` 计数器要求**连续多帧 header 字段一致**，防 ID3/JPEG 里的伪 sync。
6. **`mp3_read_header` 建 stream + 找首帧**：继承 ID3v2 tag → 解 Xing/VBRI VBR 头拿总时长 → 用 `1/14112000` 时间基建 `AVStream` → 定位到第一帧数据。

下面按这 6 步逐个钻进去。

#### 2.1.1 URL → 协议匹配

**调用链**（每行都是"谁调用谁"）：

```
avformat_open_input           libavformat/demux.c:231
  └─ init_input               libavformat/demux.c:158
       └─ s->io_open = io_open_default       options.c
            └─ ffio_open_whitelist           libavformat/avio.c:472
                 └─ ffurl_open_whitelist     libavformat/avio.c:363
                      ├─ ffurl_alloc         libavformat/avio.c:350   ← 找协议 + 分配实例
                      │    └─ url_find_protocol     libavformat/avio.c:307
                      │    └─ url_alloc_for_protocol libavformat/avio.c:118
                      └─ ffurl_connect       libavformat/avio.c:205   ← 调 prot->url_open
                           └─ prot->url_open = http_open  libavformat/http.c:769
```

**`url_find_protocol` — 查协议虚表**

```c
// libavformat/avio.c:307
static const URLProtocol *url_find_protocol(const char *filename);
```

- **参数**：`filename` —— 完整 URL，如 `"https://soundhelix.com/x.mp3"`。
- **返回**：`const URLProtocol *`，匹配不到返回 `NULL`（上层会变 `AVERROR_PROTOCOL_NOT_FOUND`）。
- **做什么**：`strspn` 数出 `"https"`，遍历 `ffurl_get_protocols()` 返回的全局协议表，`strcmp(proto_str, up->name)` 命中 `ff_http_protocol` 就返回它。没 scheme（如本地路径）默认走 `"file"`。

这里的 `URLProtocol` 是**类型定义**（虚表，全局 `static const`，一份），装的是函数指针 `url_open` / `url_read` / `url_write` / `url_seek` / `url_close` 和 `priv_data_size`。

**`url_alloc_for_protocol` — 类型 → 实例**

```c
// libavformat/avio.c:118
static int url_alloc_for_protocol(URLContext **puc, const URLProtocol *up,
                                  const char *filename, int flags,
                                  const AVIOInterruptCB *int_cb);
```

- **参数**：`puc` 出参，`up` 上一步返回的虚表，`filename`/`flags`/`int_cb` 透传。
- **返回**：`int`，`0` 成功，`AVERROR(ENOMEM)` / `AVERROR(EIO)` 失败。
- **做什么**（`libavformat/avio.c:139` 开始）：
  1. `uc = av_mallocz(sizeof(URLContext) + strlen(filename) + 1)` —— **一次 malloc 把结构体 + filename 放一起**，`uc->filename = (char*)&uc[1]` 指向尾部。
  2. `uc->prot = up` —— **绑定虚表**，后面所有 `ffurl_read/write/seek` 都通过 `uc->prot->url_read(uc, ...)` 分发，这就是 C 的手搓多态。
  3. `uc->priv_data = av_mallocz(up->priv_data_size)` —— 按协议声明的大小分配**私有状态**。HTTP 的 `priv_data` 就是 `HTTPContext`（`libavformat/http.c:67`），装 socket fd、cookies、chunk 状态、响应头等等。外层 `URLContext` 不知道里面是什么。
  4. 如果 `up->priv_data_class` 存在，把 AVClass 指针塞到 `priv_data` 开头，`av_opt_set_defaults` 填 `user_agent` 等 AVOption 默认值。

返回之后，`URLContext` 是**未连接**的空壳 —— 没 socket、没发请求。真正建连在下一步。

**`ffurl_connect` — 触发协议的 url_open**

```c
// libavformat/avio.c:205
int ffurl_connect(URLContext *uc, AVDictionary **options);
```

- **参数**：`uc` 是上一步分配好的实例，`options` 透传（如 `-user_agent`、`-headers`）。
- **返回**：`0` 成功，负值是协议返回的错误（`AVERROR(ECONNREFUSED)` / `AVERROR_HTTP_*` 等）。
- **做什么**：检查协议白黑名单，然后第 244 行**通过虚表分发**：

  ```c
  err = uc->prot->url_open2 ? uc->prot->url_open2(uc, uc->filename, uc->flags, options)
                            : uc->prot->url_open (uc, uc->filename, uc->flags);
  ```

  对 HTTP，`prot->url_open` 就是 **`http_open`**（`libavformat/http.c:769`），下一小节的入口。成功后置 `uc->is_connected = 1`。

**一句话**：这一步的产物是一个 **`URLContext *`**，里面有连接好的 socket（通过 `priv_data` 里的 `HTTPContext`），下一步要把它包进 `AVIOContext`。

#### 2.1.2 HTTP 连接建立

**调用链**：

```
http_open                    libavformat/http.c:769    ← uc->prot->url_open
  └─ http_open_cnx           libavformat/http.c:403
       └─ http_open_cnx_internal  libavformat/http.c:228
            ├─ av_url_split                  ← 拆 URL
            ├─ ffurl_open_whitelist          ← 递归打开 tls:// (HTTPS) 或 tcp://
            │    └─ [tls_open → tcp_open]    libavformat/tcp.c:149
            └─ http_connect    libavformat/http.c:1523   ← 拼 GET 请求 + 收响应头
                 ├─ ffurl_write              ← 发出 request
                 └─ http_read_header         libavformat/http.c:1434
                      └─ process_line        libavformat/http.c:1172   ← 解析每一行响应头
```

**`http_open` — 协议入口**

```c
// libavformat/http.c:769
static int http_open(URLContext *h, const char *uri, int flags,
                     AVDictionary **options);
```

- **参数**：`h` 是 2.1.1 分配的 `URLContext`，`uri` 同 `h->filename`，`flags` = `AVIO_FLAG_READ`。
- **返回**：`0` 成功，负值错误。
- **做什么**（第 775-811 行）：
  - 根据 `s->seekable` 初始化 `h->is_streamed`（下面第 3 步还会被响应头覆盖）。
  - `s->filesize = UINT64_MAX`，`s->location = av_strdup(uri)` 记住 URL 用于后面重定向。
  - 调 `http_open_cnx(h, options)` 进入真正的连接逻辑。

**`http_open_cnx_internal` — 拆 URL + 递归打开下层协议**

```c
// libavformat/http.c:228
static int http_open_cnx_internal(URLContext *h, AVDictionary **options);
```

- **返回**：`0` 成功（最后转发给 `http_connect` 的返回值），负值错误。
- **做什么**：
  1. `av_url_split(proto, ..., hostname, &port, path1, ..., s->location)` 拆出 `proto`/`hostname`/`port`/`path`。
  2. **决定下层协议**：`https` → `lower_proto = "tls"`, `port = 443`；`http` → 保持 `tcp`, `port = 80`。
  3. `ff_url_join(buf, ..., lower_proto, NULL, hostname, port, NULL)` 拼出 `"tls://host:443"` 或 `"tcp://host:80"`。
  4. `ffurl_open_whitelist(&s->hd, buf, AVIO_FLAG_READ_WRITE, ...)` —— **递归调用整个 2.1.1 流程**打开下层：`tls://` 协议再递归到 `tcp://`，TCP 协议（`libavformat/tcp.c:149-240`）用 `getaddrinfo` 解 DNS，`ff_connect_parallel` 同时试 IPv4/IPv6 并发连接，谁先成功用谁。`s->hd` 就是下层的 `URLContext *`。
  5. 最后 `return http_connect(h, path, local_path, hoststr, auth, proxyauth)`。

**协议栈的精髓**：`http → tls → tcp → socket`，**每一层都是一个 URLContext**，上层通过 `ffurl_write(s->hd, ...)` / `ffurl_read(s->hd, ...)` 读写下层，不关心下层是裸 TCP 还是 TLS。

**`http_connect` — 拼 GET + 解析响应头**

```c
// libavformat/http.c:1523
static int http_connect(URLContext *h, const char *path, const char *local_path,
                        const char *hoststr, const char *auth, const char *proxyauth);
```

- **返回**：`0` 成功，`AVERROR_HTTP_*` 或 socket 错误。
- **做什么**：
  1. **拼请求**（第 1580 行附近）：

     ```c
     av_bprintf(&request, "%s ", method);          // "GET "
     bprint_escaped_path(&request, path);           // "/x.mp3"
     av_bprintf(&request, " HTTP/1.1\r\nHost: %s\r\n", hoststr);
     av_bprintf(&request, "User-Agent: %s\r\n", s->user_agent);
     if (!post && (s->off > 0 || s->end_off || s->seekable != 0))
         av_bprintf(&request, "Range: bytes=%"PRIu64"-\r\n", s->off);
     av_bprintf(&request, "Connection: %s\r\n\r\n",
                s->multiple_requests ? "keep-alive" : "close");
     ffurl_write(s->hd, request.str, request.len);  // 通过下层 tls/tcp 发出
     ```

     **重点**：就算从头读，也会发 `Range: bytes=0-`。目的是**主动试探服务器支持不支持 Range** —— 若服务器回 `206 Partial Content` + `Content-Range`，说明这个资源可以 seek。

  2. **读响应头**：`http_read_header(h)`（`libavformat/http.c:1434`）循环 `http_get_line` 读行，每行交给 `process_line`（`libavformat/http.c:1172`）：

     ```c
     // libavformat/http.c:1172
     if (!av_strcasecmp(tag, "Content-Length"))
         s->filesize = strtoull(p, NULL, 10);
     else if (!av_strcasecmp(tag, "Content-Range"))
         parse_content_range(h, p);                    // "bytes 0-99/100"
     else if (!av_strcasecmp(tag, "Accept-Ranges")) {
         if (!strncmp(p, "bytes", 5))
             h->is_streamed = 0;                       // 标记可 seek
     } else if (!av_strcasecmp(tag, "Transfer-Encoding")) {
         if (!av_strncasecmp(p, "chunked", 7))
             s->filesize = UINT64_MAX;                 // chunked → 未知大小
     }
     ```

     `h->is_streamed = 0` 意思是**可以 seek**（命名反直觉：`is_streamed=1` 才是"只能流式读"）。这个字段会传上去变成 `AVIOContext::seekable`，决定 `avformat_seek_file` 能不能跳。

**这一小节的产物**：一个**已连接、已发请求、已读完响应头**的 `URLContext *`，`filesize` / `is_streamed` / `content_type` 都填好了。

#### 2.1.3 URLContext → AVIOContext 的封装

**为什么需要这一步**：上层 demuxer（`mp3_read_header` / `mp3_read_packet` 等）只会调 `avio_read(s->pb, ...)` / `avio_seek(...)`，参数是 `AVIOContext *` 而不是 `URLContext *`。两者的职责不同：`URLContext` 只管"把字节从某处搬过来"，`AVIOContext` 在它上面加一层**用户态读写缓冲 + 统一回调**。

**调用链**：

```
ffurl_open_whitelist (返回后继续)
  ↑
ffio_open_whitelist    libavformat/avio.c:472       ← s->io_open 的实际实现
  └─ ffio_fdopen       libavformat/avio.c:413       ← URLContext → AVIOContext
       └─ avio_alloc_context            aviobuf.c
```

**`ffio_fdopen` — 套缓冲并接回调**

```c
// libavformat/avio.c:413
int ffio_fdopen(AVIOContext **sp, URLContext *h);
```

- **参数**：`sp` 出参，`h` 上一步的 `URLContext`（HTTP 连接已建）。
- **返回**：`0` 成功，`AVERROR(ENOMEM)` 失败。
- **做什么**：
  1. 算缓冲大小：默认 `IO_BUFFER_SIZE = 32KB`；若不可 seek（`h->is_streamed`）且是读，`*= 2` 变 64KB（流式读需要更大的环形 buffer 供回退/parser）。
  2. `buffer = av_malloc(buffer_size)` 分配 IO 缓冲。
  3. **`avio_alloc_context`** 创建 `AVIOContext`：

     ```c
     *sp = avio_alloc_context(buffer, buffer_size, write_flag,
                              h,                  // opaque = URLContext
                              ffurl_read2,        // read_packet 回调
                              ffurl_write2,       // write_packet 回调
                              ffurl_seek2);       // seek 回调
     ```

  4. 把 `URLContext` 的属性抄到 `AVIOContext`：
     - `s->seekable = h->is_streamed ? 0 : AVIO_SEEKABLE_NORMAL`（**反转语义**）。
     - `s->max_packet_size`、`s->min_packet_size`、`s->read_pause`、`s->read_seek`（后两个来自 `h->prot`，RTSP 之类才用到）。
  5. `s->av_class = &ff_avio_class` 打开 AVOption 访问，后面 `av_opt_get(pb, "mime_type", ...)` 能拿到 HTTP 的 `Content-Type`。

**三个回调的实现**（`libavformat/avio.c:549-556` 附近）是 `URLContext` 虚表的薄包装：

```c
int ffurl_read2(void *urlcontext, uint8_t *buf, int size) {
    return retry_transfer_wrapper((URLContext *)urlcontext, buf, NULL, size, 1, 1);
    // retry_transfer_wrapper 内部最终调 h->prot->url_read(h, buf, size)
}

int64_t ffurl_seek2(void *urlcontext, int64_t pos, int whence) {
    URLContext *h = urlcontext;
    if (!h->prot->url_seek) return AVERROR(ENOSYS);
    return h->prot->url_seek(h, pos, whence);          // → http_seek
}
```

**精髓**：从这一刻起，上层 demuxer 调 `avio_read(s->pb, ...)`，走的是 `s->pb->read_packet = ffurl_read2 → h->prot->url_read = http_read`，**完全不知道字节来自 HTTP 还是本地文件**。本地文件走 `file_read`（包 `read(2)` 系统调用），HTTP 走 `http_read`（包 TCP + chunked 解码）。两者都能被同一个 mp3 demuxer 吃。这就是为什么 `audio_flow/main.c:59` 的 `avformat_open_input` 对 URL 和本地路径代码路径完全一样。

**产物**：一个 `AVIOContext *`，挂在 `AVFormatContext::pb` 上，`init_input` 返回。

#### 2.1.4 格式探测

**调用链**：

```
avformat_open_input             libavformat/demux.c:231
  └─ init_input                 libavformat/demux.c:158
       └─ av_probe_input_buffer2    libavformat/format.c:256   ← 指数增长外循环
            └─ av_probe_input_format2   libavformat/format.c:238
                 └─ av_probe_input_format3  libavformat/format.c:156  ← 遍历所有 demuxer 打分
                      └─ ifmt->read_probe        ← 如 mp3_read_probe
```

**`av_probe_input_buffer2` — 指数增长的探测循环**

```c
// libavformat/format.c:256
int av_probe_input_buffer2(AVIOContext *pb, const AVInputFormat **fmt,
                           const char *filename, void *logctx,
                           unsigned int offset, unsigned int max_probe_size);
```

- **参数**：`pb` 刚封好的 AVIOContext，`fmt` 出参（选中的 demuxer），`offset` 起始偏移，`max_probe_size` 上限（默认 `PROBE_BUF_MAX = 1MB`）。
- **返回**：`≥0` 是最终的 probe score，负值是错误（`AVERROR_INVALIDDATA` / `AVERROR_EOF` 等）。
- **做什么**（第 289-333 行）：

  ```c
  for (probe_size = PROBE_BUF_MIN;              // 2048
       probe_size <= max_probe_size && !*fmt && !eof;
       probe_size = FFMIN(probe_size << 1, ...)) {   // 2K → 4K → 8K → ... → 1MB
      av_reallocp(&buf, probe_size + AVPROBE_PADDING_SIZE);
      ret = avio_read(pb, buf + buf_offset, probe_size - buf_offset);  // 读新增那一截
      pd.buf = &buf[offset]; pd.buf_size = buf_offset - offset;
      *fmt = av_probe_input_format2(&pd, 1, &score);   // 打分
      if (*fmt && score > AVPROBE_SCORE_RETRY) break;  // 有够确定的了就停
  }
  // 最后 ffio_rewind_with_probe_data：把读过的字节重新塞回 pb 的缓冲，不做真正 seek
  ```

  核心是**倍增策略**：2KB 够认多数容器，不够就 4KB、8KB……一直到 1MB。探测完**不 seek** —— 而是把 buffer 塞回 AVIOContext 的内部缓冲里（`ffio_rewind_with_probe_data`），对流式 HTTP 也管用。

**`av_probe_input_format3` — 三路打分**

```c
// libavformat/format.c:156
const AVInputFormat *av_probe_input_format3(const AVProbeData *pd,
                                            int is_opened, int *score_ret);
```

- **参数**：`pd` 包含 `buf`/`buf_size`/`filename`/`mime_type`，`is_opened=1` 表示 AVIOContext 已打开（过滤掉 `AVFMT_NOFILE` 格式）。
- **返回**：得分最高的 `AVInputFormat *`，`*score_ret` 是分数。
- **做什么**（第 191-230 行循环）：

  ```c
  while ((fmt1 = av_demuxer_iterate(&i))) {          // 遍历所有编译进来的 demuxer
      score = 0;
      if (ffifmt(fmt1)->read_probe)
          score = ffifmt(fmt1)->read_probe(&lpd);     // ← 具体 demuxer 的打分 (见 2.1.5)
      if (fmt1->extensions && av_match_ext(lpd.filename, fmt1->extensions))
          score = FFMAX(score, 1);                    // 扩展名匹配兜底 1 分
      if (av_match_name(lpd.mime_type, fmt1->mime_type))
          score += AVPROBE_SCORE_MIME_BONUS;          // HTTP MIME 加分
      if (score > score_max) { score_max = score; fmt = fmt1; }
      else if (score == score_max) fmt = NULL;        // 并列 → 模棱两可，不选
  }
  ```

  **三路信号**：内容特征（`read_probe`）+ 文件扩展名 + HTTP Content-Type。MIME 加成对 HTTP 场景很关键 —— 服务器明说 `audio/mpeg` 就倾向 MP3。**并列视为不确定**（`fmt = NULL`），会让外层继续读更多字节再试。

#### 2.1.5 MP3 probe 的连续帧校验

**被谁调用**：2.1.4 的 `av_probe_input_format3` 循环里 `ffifmt(fmt1)->read_probe(&lpd)`，对 mp3 demuxer 来说就是 `mp3_read_probe`。

**`mp3_read_probe`**

```c
// libavformat/mp3dec.c:68
static int mp3_read_probe(const AVProbeData *p);
```

- **参数**：`p->buf` 是 probe buffer，`p->buf_size` 是字节数。
- **返回**：`0 ~ AVPROBE_SCORE_EXTENSION+1` 的打分，**越大越确定是 MP3**。
- **为什么复杂**：MP3 没有全局 magic bytes（ID3v2 有，但不是所有 MP3 都带 ID3v2），只能靠**连续多帧 header 校验** + **假 sync 统计**两道防线。

核心代码：

```c
// libavformat/mp3dec.c:86
for (; buf < end; buf = buf2+1) {                    // 尝试每个可能的起点
    buf2 = buf;
    for (framesizes = frames = 0; buf2 < end; frames++) {
        MPADecodeHeader h;
        int header_emu = 0;
        header = AV_RB32(buf2);
        ret = avpriv_mpegaudio_decode_header(&h, header);   // 解 32-bit header
        if (ret != 0) break;                          // 头不合法，这个位置不是 MP3 帧

        // 【内层防误判】在这一帧 payload 里扫"看起来像 header 但其实是压缩音频"的假 sync
        available = FFMIN(h.frame_size, end - buf2);
        for (buf3 = buf2 + 4; buf3 < buf2 + available; buf3++) {
            uint32_t next_sync = AV_RB32(buf3);
            header_emu += (next_sync & MP3_MASK) == (header & MP3_MASK);
        }
        if (header_emu > 2) break;                    // 假 sync 太多 → 整段作废

        framesizes += h.frame_size;
        buf2 += h.frame_size;                          // 跳到下一帧继续验
    }
    max_frames = FFMAX(max_frames, frames);           // 记录最多能连上几帧
    if (buf == buf0) first_frames = frames;           // 文件开头能连几帧（最高权重）
}

// 打分阶梯（libavformat/mp3dec.c:122-129）
if      (first_frames >= 7)                          return AVPROBE_SCORE_EXTENSION + 1;
else if (max_frames > 200 && ...)                    return AVPROBE_SCORE_EXTENSION;
else if (max_frames >= 4 && ...)                     return AVPROBE_SCORE_EXTENSION / 2;
else if (ff_id3v2_match(buf0, ID3v2_DEFAULT_MAGIC))  return ...;
else if (first_frames > 1 && whole_used)             return 5;
else if (max_frames >= 1)                            return 1;
else                                                 return 0;
```

**两层防误判**：

1. **外层连续帧**：从某个偏移开始要能连续解出 N 帧合法 header（`avpriv_mpegaudio_decode_header` 检查 sync word + 所有字段组合），N 越大越可信。
2. **内层 `header_emu`**：在一帧的 payload 字节里扫**看起来像 sync 但不是 header 开始**的位置。真实 MP3 的压缩音频字节在统计上不会频繁匹配到 `0xFFE0` 这种模式；如果扫出 `> 2` 个假 sync，说明这段字节更像 ID3 tag / JPEG 封面 / 随机字节，整段作废。

没这两层校验，`0xFFFB` 这样的字节组合在 ID3 / JPEG / 文本里出现概率不低，MP3 probe 会疯狂误报。

#### 2.1.6 `mp3_read_header` — 建 stream + 找首帧

**被谁调用**：`avformat_open_input`（`libavformat/demux.c:322-327`）在 probe 选出 mp3 demuxer 后执行：

```c
// libavformat/demux.c:322
if (ffifmt(s->iformat)->read_header)
    if ((ret = ffifmt(s->iformat)->read_header(s)) < 0)   // ← 这里
        ...
```

对 mp3 demuxer 就是 `mp3_read_header`。

**`mp3_read_header`**

```c
// libavformat/mp3dec.c:372
static int mp3_read_header(AVFormatContext *s);
```

- **参数**：`s` —— 已经打开 pb 并选中 mp3 demuxer 的 `AVFormatContext`。
- **返回**：`0` 成功，负值失败。
- **做什么**（依次 7 步）：

  1. **继承 ID3v2 metadata**（第 382-383 行）：

     ```c
     s->metadata = si->id3v2_meta;
     si->id3v2_meta = NULL;
     ```

     `si->id3v2_meta` 是 `avformat_open_input` 在调 `read_header` 之前先跑 `ff_id3v2_read_dict`（`libavformat/demux.c:319-320`）收集的 tag，mp3 demuxer 直接接过来做自己的 `s->metadata`。

  2. **建流 + 设 codec**（第 385-391 行）：

     ```c
     st = avformat_new_stream(s, NULL);
     st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
     st->codecpar->codec_id   = AV_CODEC_ID_MP3;
     sti->need_parsing        = AVSTREAM_PARSE_FULL_RAW;  // parser 负责切帧
     st->start_time           = 0;
     ```

     注意：`sample_rate` / `ch_layout` 这些**不填** —— 它们要等 parser 真的解第一帧 header 才能拿到（`avformat_find_stream_info` 干的活）。

  3. **设时间基**（第 396 行）：

     ```c
     avpriv_set_pts_info(st, 64, 1, 14112000);
     ```

     `14112000 = LCM(8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000)` —— MP3 所有合法采样率的最小公倍数。用这个时间基，任何采样率下的 PTS 都是整数，不丢精度。

  4. **读 ID3v1 尾部 tag**（第 401-402 行）：如果 ID3v2 里没有内容，尝试文件末尾的 ID3v1（128 字节定长 tag）。

  5. **Xing / VBRI VBR 头**（第 407 行 `mp3_parse_vbr_tags`）：VBR MP3 没有全局 duration，第一帧的 padding 区域会放 Xing 或 VBRI 头，里面写总帧数 `mp3->frames`、总字节数、TOC seek 表。用它算出 `s->duration`；没有就只能 `bitrate × filesize` 估算。

  6. **找首帧数据起点**（第 418-437 行 —— 这是本函数最硬核的部分）：

     ```c
     off = avio_tell(s->pb);
     for (i = 0; i < 64 * 1024; i++) {               // 最多扫 64KB 垃圾字节
         uint32_t header, header2;
         int frame_size = check(s->pb, off + i, &header);   // 解 off+i 处的 header
         if (frame_size > 0) {
             ret = check(s->pb, off + i + frame_size, &header2);  // 再解下一帧
             if (ret >= 0 && (header & MP3_MASK) == (header2 & MP3_MASK))
                 break;                              // 相邻两帧 sync bits 一致 → 认
         } else if (frame_size == CHECK_SEEK_FAILED) {
             return AVERROR_INVALIDDATA;
         }
     }
     off = avio_seek(s->pb, off + i, SEEK_SET);       // 定位到首帧
     ```

     **为什么要扫两帧**：单帧 header 可能是 ID3 tag 或 cover art 里的假 sync。要求 "相邻两帧 sync bits 一致" 才认，和 2.1.5 的思路一样 —— 连续性是 MP3 最强的结构信号。

  7. **修正 TOC 索引**（第 442-443 行）：Xing TOC 里的位置是**相对于 VBR header 后面的**，加上 `off` 变成绝对文件偏移。

**返回时的状态**：
- `AVFormatContext::streams[0]` 是一个 `codec_id = AV_CODEC_ID_MP3` 的音频流，`codec_type`/`time_base`/`start_time` 已填。
- `sample_rate` / `ch_layout` / `sample_fmt` **还是空** —— 要等 §2.2 的 `avformat_find_stream_info` 去真解一帧才知道。
- `s->pb` 定位在**第一帧 MP3 数据**，后续 `av_read_frame` 从这里开始读。

---

### 2.2 `avformat_find_stream_info` — 把 codec 参数补全

`libavformat/demux.c:2607`。**为什么还要再探一次？**

`avformat_open_input` 只读容器头。对很多格式（Raw MP3、MPEG-TS、FLV、带 `AVFMTCTX_NOHEADER` 的格式），关键 codec 参数 —— sample_rate、channels、sample_fmt、H.264 SPS/PPS —— 散在真正的帧数据里，不解一两帧根本拿不到。find_stream_info 的职责就是**真解几帧，把这些参数填出来**。

主循环（`libavformat/demux.c:2722-2944`）是这样的：

```c
for (;;) {
    ret = read_frame_internal(ic, pkt1);              // 真读一个 packet
    // 关键：读出来的不扔，按默认保存到 packet_buffer
    if (!(ic->flags & AVFMT_FLAG_NOBUFFER)) {
        avpriv_packet_list_put(&si->packet_buffer, pkt1, NULL, 0);
        pkt = &si->packet_buffer.tail->pkt;            // 拿刚塞进去的那份引用
    }
    // 送到 try_decode_frame 真解
    try_decode_frame(ic, st, pkt, ...);                // libavformat/demux.c:2138
    sti->codec_info_nb_frames++;

    // 三种退出条件
    if (所有流的 codec 参数都齐了) break;
    if (read_size >= probesize) break;                 // 默认 5MB
    if (本流累计时长 > max_analyze_duration) break;    // 默认 5s~90s
}
```

`try_decode_frame`（`libavformat/demux.c:2138-2238`）内部的关键动作：

```c
if (!avcodec_is_open(sti->avctx)) {
    av_dict_set(&options, "threads", "1", 0);         // libavformat/demux.c:2169
    avcodec_open2(sti->avctx, codec, &options);
}
avcodec_send_packet(sti->avctx, pkt);                  // 2206
avcodec_receive_frame(sti->avctx, frame);              // 2211
sti->nb_decoded_frames++;
```

**`threads = 1` 强制**的注释原话是："Force thread count to 1 since the H.264 decoder will not extract SPS and PPS to extradata during multi-threaded decoding." —— H.264 多线程解码模式下 SPS/PPS 不会被正确写进 `extradata`，导致后续真正创建解码器时拿不到 codec 参数。音频解码本来就不开多线程（见 §2.10），这个限制对音频没实际影响，但作用到所有流。

**三个退出条件**的默认值：
- `probesize`：5MB（字节数）—— 硬上限
- `max_analyze_duration`：5 秒（流默认），某些容器 90 秒
- "所有流参数都齐了"：最宽松，一般 2-3 帧就够

**最关键的细节：读过的 packet 不会被丢**。`libavformat/demux.c:2811` 那行 `avpriv_packet_list_put` 把每个读出的 packet 都塞到 `si->packet_buffer` 链表。**find_stream_info 返回后，用户第一次调 `av_read_frame` 时，会先吐这些缓存里的 packet，然后才从 IO 读新的**。这意味着 find_stream_info 消耗的那 5MB 不是浪费，对用户透明。只有设了 `AVFMT_FLAG_NOBUFFER` 的场景（如超低延迟直播）才丢弃。

---

### 2.3 `av_read_frame` — 取下一个 AVPacket

`libavformat/demux.c:1588`。这是最容易被当作"黑盒"的函数，但**内部实际上是两层缓冲 + 三层抽象**：

```
av_read_frame
  ↓
read_frame_internal    ← GENPTS 重排 / parser 拆包
  ↓
ff_read_packet          ← raw_packet_buffer + FFERROR_REDO 循环
  ↓
iformat->read_packet    ← demuxer 私有（如 mp3_read_packet）
  ↓
avio_read               ← AVIOContext 回调
  ↓
ffurl_read → prot->url_read  ← 协议层 (http_read / file_read)
```

#### 2.3.1 两个缓冲的职责

- **`raw_packet_buffer`**（在 `ff_read_packet` 里，`libavformat/demux.c:645-656`）：**探测阶段**的 packet 缓冲。codec_id 还没确定前，demuxer 读出来的 packet 先塞这里，等 probe 完成才放出来。
- **`packet_buffer`**（在 `av_read_frame` 和 `find_stream_info` 共用，`libavformat/demux.c:1597, 2811`）：**两个用途共用一个缓冲**：
  1. `find_stream_info` 读过的 packet 都存这里，供后续 `av_read_frame` 消耗
  2. `GENPTS` 模式下为 PTS 推算做乱序重排

#### 2.3.2 GENPTS 路径

```c
// libavformat/demux.c:1588
int av_read_frame(AVFormatContext *s, AVPacket *pkt) {
    const int genpts = s->flags & AVFMT_FLAG_GENPTS;
    if (!genpts) {
        // 简单路径
        return si->packet_buffer.head
            ? avpriv_packet_list_get(&si->packet_buffer, pkt)
            : read_frame_internal(s, pkt);
    }
    // GENPTS 路径
    for (;;) {
        ret = read_frame_internal(s, pkt);
        compute_pkt_fields(s, st, NULL, pkt, ...);     // 推算 PTS
        avpriv_packet_list_put(&si->packet_buffer, pkt, ...);
        // 扫 packet_buffer，找"最早那个 PTS 已经能确定"的 packet pop
        if (条件满足) break;
    }
    return avpriv_packet_list_get(&si->packet_buffer, pkt);
}
```

**GENPTS 做什么**：有些容器（早期 AVI、MPEG-PS）只带 DTS 不带 PTS。对视频的 B-frame，DTS 和 PTS 顺序不一样，得缓冲多个 packet 才能反推回正确的 PTS。对音频（无 B-frame）这条路径基本是空转 —— 但对面试时说"我看过 av_read_frame 源码"加不少分。

#### 2.3.3 `FFERROR_REDO`

`ff_read_packet`（`libavformat/demux.c:629-691`）的循环里：

```c
for (;;) {
    ret = s->iformat->read_packet(s, pkt);             // demuxer 私有
    if (ret == FFERROR_REDO) continue;                  // libavformat/demux.c:666
    // ...
}
```

**什么时候 demuxer 返回 `FFERROR_REDO`？** 它"消化了一段数据但不产出 packet"的时候 —— 比如读到 ID3v2 tag、PMT 表、filler bytes、损坏的数据需要跳过。demuxer 不能返回 0（会被当作成功），也不能返回错误（会中止读取），所以定义了这个**内部错误码**（不对外暴露）表示"再调我一次"。

#### 2.3.4 `compute_pkt_fields` 做什么

`libavformat/demux.c:983-1170+`。这个函数从 parser 状态、上一个 packet 的 dts、stream 的 `cur_dts` / `last_IP_pts` 里**推算出缺失的 PTS/DTS/duration**，还处理 **33-bit MPEG-TS 时间戳环绕**：

```c
// libavformat/demux.c:1040
if (pkt->dts != AV_NOPTS_VALUE &&
    (pkt->dts & ((1ULL << st->pts_wrap_bits) - 1)) < ...)
    pkt->dts -= 1ULL << st->pts_wrap_bits;             // 时间戳 wrap 修正
```

`st->pts_wrap_bits` 对 MPEG-TS 是 33（PCR 是 33 位），对 MP4 通常 64（不会 wrap）。

#### 2.3.5 Parser 拆包

有些 codec 需要二次拆包：demuxer 给的一个 packet 可能包含半帧或几帧，必须交给 `av_parser_parse2` 按帧边界切开。典型：**AAC in MPEG-TS**（TS 包 188 字节固定大小，AAC 帧跨 TS 包）、**MPEG-1 audio in VOB**。

```c
// libavformat/demux.c:1178 parse_packet
len = av_parser_parse2(sti->parser, sti->avctx,
                       &out_pkt->data, &out_pkt->size,
                       data, size, pkt->pts, pkt->dts, pkt->pos);
// parser 从 data 里拆出一个完整帧，用 parser 内部状态填 pts/dts
out_pkt->pts = sti->parser->pts;
out_pkt->dts = sti->parser->dts;
avpriv_packet_list_put(&si->parse_queue, out_pkt, NULL, 0);
```

切出的帧排进 `parse_queue`。MP3 的常见容器（裸 MP3、MP4）都对齐到帧边界，所以不走 parser；但 **MP3 in MPEG-TS** 需要。

#### 2.3.6 MP3 demuxer 的 `read_packet`

`libavformat/mp3dec.c:451-473`：

```c
static int mp3_read_packet(AVFormatContext *s, AVPacket *pkt) {
    MP3DecContext *mp3 = s->priv_data;
    int size = MP3_PACKET_SIZE;                        // 通常 4096
    int64_t pos = avio_tell(s->pb);
    if (mp3->filesize > ID3v1_TAG_SIZE && pos < mp3->filesize)
        size = FFMIN(size, mp3->filesize - ID3v1_TAG_SIZE - pos);
    ret = av_get_packet(s->pb, pkt, size);
    pkt->flags &= ~AV_PKT_FLAG_CORRUPT;
    pkt->stream_index = 0;
    return ret;
}
```

**非常简单**：固定 4KB 一次读下去。不做帧边界对齐，不判断合法 header。理由：`need_parsing = AVSTREAM_PARSE_FULL_RAW`，**上层的 parser 会再切一次**，按真实帧边界重组。这让 demuxer 保持极简，复杂度推给 parser。

---

### 2.4 `avcodec_send_packet` — 把压缩包推给解码器

`libavcodec/decode.c:719`。push 模型的关键一半。**内部只有 1 个 packet + 1 个 frame 的缓冲**，不是队列：

```c
// libavcodec/decode.c:719
int avcodec_send_packet(AVCodecContext *avctx, const AVPacket *avpkt) {
    AVCodecInternal *avci = avctx->internal;
    DecodeContext    *dc  = decode_ctx(avci);

    if (dc->draining_started)                           // 已经 flush 过
        return AVERROR_EOF;                              // 729

    if (avpkt && (avpkt->data || avpkt->side_data_elems)) {
        if (!AVPACKET_IS_EMPTY(avci->buffer_pkt))       // buffer_pkt 被占
            return AVERROR(EAGAIN);                      // 736
        ret = av_packet_ref(avci->buffer_pkt, avpkt);   // 737: 零拷贝 ref
        if (ret < 0) return ret;
    } else {
        dc->draining_started = 1;                        // NULL = flush 信号
    }

    // 主动推进：立刻尝试解一帧，填 buffer_frame
    if (!avci->buffer_frame->buf[0] && !dc->draining_started) {
        ret = decode_receive_frame_internal(avctx, avci->buffer_frame, 0);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
            return ret;
    }
    return 0;
}
```

#### 2.4.1 内部状态

`AVCodecInternal`（`libavcodec/internal.h:144-146`）+ `DecodeContext`（`libavcodec/decode.c:61-84`）里几个关键字段：

```c
AVPacket *buffer_pkt;          // 刚 send 进来还没消化的 packet
AVFrame  *buffer_frame;        // 预填好的帧，等 receive 拿走
int       draining_started;    // 用户 send 了 NULL 没？ (DecodeContext.84)
int       draining_done;       // 解码器已经返回 EOF？ (AVCodecInternal.146)
```

#### 2.4.2 `av_packet_ref` 的零拷贝

`av_packet_ref` 不是 memcpy，是**把 `avpkt->buf` 这个 `AVBufferRef` 原子 +1**，让 `buffer_pkt` 和用户传进来的 packet 共享同一块内存。用户可以立刻 `av_packet_unref(avpkt)`，数据不会被释放（refcount 还 > 0）。解码器消耗 bytes 时通过 `pkt->data += consumed` / `pkt->size -= consumed` 推进指针（`decode_simple_internal` 里 `libavcodec/decode.c:500-501`），**不改动原始缓冲**。

#### 2.4.3 为什么 send 主动调 decode_receive_frame_internal

上面代码 `libavcodec/decode.c:743-747` 那段：send 成功后，如果 `buffer_frame` 还是空的，立刻尝试解一帧填上。

**为什么？** 因为 push 模型的常见调用模式是：

```c
while (av_read_frame(fmt, pkt) >= 0) {
    avcodec_send_packet(dec, pkt);
    while (avcodec_receive_frame(dec, frame) >= 0) {
        // 用 frame
    }
}
```

如果 send 完不预解，那么第一次 receive 还要再走一遍解码路径 —— 多一次函数调用栈切换。主动推进让"send 完立刻 receive 能拿到帧"成为**最短路径**，减少延迟。

#### 2.4.4 EAGAIN 的方向性

同一个错误码，两个函数的含义**完全相反**：

| 函数 | EAGAIN 含义 |
|---|---|
| `send_packet` | "buffer_pkt 被占了，你得先 receive_frame 把 buffer_frame 取走" |
| `receive_frame` | "buffer_frame 空的，你得先 send_packet 喂更多" |

**`libavcodec/avcodec.h:164` 的注释原话**："A codec is not allowed to return AVERROR(EAGAIN) for both sending and receiving. This would be an invalid state, which could put the codec user into an endless loop." —— 规范**硬保证**两函数不会同时返回 EAGAIN，否则用户 loop 会死锁。

#### 2.4.5 `draining_started` 是单向开关

`libavcodec/decode.c:741` 设 1，`libavcodec/decode.c:728` 检查。一旦 send(NULL)，之后所有非 NULL send 都直接返 `AVERROR_EOF`。唯一的重置方式是 `avcodec_flush_buffers`（`libavcodec/decode.c:2338-2354`），它重置 `draining_started = 0`、清空 `buffer_pkt` / `buffer_frame`、重置 PTS correction 计数器。**这是"复用解码器解另一段"的唯一合法途径** —— seek 后必须 flush，否则 decoder 仍认为自己已到流末尾。

---

### 2.5 `avcodec_receive_frame` — 把解好的 frame 取出来

入口在 `libavcodec/avcodec.c:723`，实际干活的是 `ff_decode_receive_frame`（`libavcodec/decode.c:806`）：

```c
int ff_decode_receive_frame(AVCodecContext *avctx, AVFrame *frame) {
    AVCodecInternal *avci = avctx->internal;
    int ret;
    if (avci->buffer_frame->buf[0]) {
        av_frame_move_ref(frame, avci->buffer_frame);  // 812: 零拷贝移动
        return 0;
    }
    ret = decode_receive_frame_internal(avctx, frame, flags);
    // ...
}
```

**`av_frame_move_ref`** 不是 memcpy —— 把 `buffer_frame` 的所有 `buf[]` 指针、`data[]`、`extended_data`、metadata 整体转移给 `frame`，`buffer_frame` 清零（但不释放底层数据）。纯指针操作，O(1)。

#### 2.5.1 `decode_receive_frame_internal` 的两条分支

`libavcodec/decode.c:614-648`。**codec 分两大类**：

```c
if (codec->cb_type == FF_CODEC_CB_TYPE_RECEIVE_FRAME) {
    // 现代 API：codec 自己维护状态机
    while (1) {
        ret = codec->cb.receive_frame(avctx, frame);  // 627
        if (!ret && avctx->codec->type == AVMEDIA_TYPE_AUDIO)
            ret = discard_samples(avctx, frame, &discarded_samples);
        if (ret == AVERROR(EAGAIN) || (frame->flags & AV_FRAME_FLAG_DISCARD)) {
            av_frame_unref(frame);
            continue;                                   // 静默重试
        }
        break;
    }
} else {
    // 传统 API (FF_CODEC_CB_TYPE_DECODE)
    ret = decode_simple_receive_frame(avctx, frame);   // 642
}
```

两种 `cb_type` 定义在 `libavcodec/codec_internal.h:106-125`：
- **`FF_CODEC_CB_TYPE_DECODE`**：传统 API，一次 call 返回"消耗了多少字节 + 是否产出帧"。**MP3、H.264、大部分老 codec 用这个**。
- **`FF_CODEC_CB_TYPE_RECEIVE_FRAME`**：新 API，一次 call 要么返回一帧要么 EAGAIN。libdav1d、libopus、硬件解码器用这个。

#### 2.5.2 `decode_simple_internal` 的 packet 消耗模型

`libavcodec/decode.c:417-511`。传统 API 的核心：

```c
if (!pkt->data && !avci->draining) {
    av_packet_unref(pkt);
    ret = ff_decode_get_packet(avctx, pkt);             // 从 buffer_pkt 取
    if (ret < 0 && ret != AVERROR_EOF) return ret;
}
got_frame = 0;
consumed = codec->cb.decode(avctx, frame, &got_frame, pkt);  // 446: 真解码

// 部分消耗时 advance 指针，不 memcpy
if (consumed >= pkt->size || ret < 0) {
    av_packet_unref(pkt);
} else {
    pkt->data += consumed;                              // 500
    pkt->size -= consumed;
    pkt->pts = AV_NOPTS_VALUE;                          // 余下部分已经不是完整帧
    pkt->dts = AV_NOPTS_VALUE;
}
```

**部分消耗的典型场景**：一个 packet 里装多帧（比如 Vorbis 或某些容器里的多帧 AAC）。decoder 返回 consumed 说明用了多少字节，下次调用继续从 `pkt->data + consumed` 解。**零内存拷贝**，靠指针前进。

---

### 2.6 MP3 解码器内部（`mpegaudiodec_template.c`）

进到 MP3 的 `decode` 回调，`libavcodec/mpegaudiodec_template.c:1557`：

```c
// 这个文件是模板，被 mpegaudiodec_float.c 和 mpegaudiodec_fixed.c include
static int decode_frame(AVCodecContext *avctx, AVFrame *frame,
                        int *got_frame_ptr, AVPacket *avpkt) {
    MPADecodeContext *s = avctx->priv_data;
    const uint8_t *buf = avpkt->data;
    int buf_size = avpkt->size;

    while (buf_size && !*buf) { buf++; buf_size--; skipped++; }  // 1567: 跳前导零
    if (AV_RB32(buf) >> 8 == AV_RB32("TAG") >> 8)       // 1577
        return buf_size + skipped;                       // 丢 ID3 tag

    header = AV_RB32(buf);
    ret = avpriv_mpegaudio_decode_header((MPADecodeHeader *)s, header);
    // ch_layout 可能每帧变化（mono↔stereo 切换）
    av_channel_layout_uninit(&avctx->ch_layout);        // 1591
    avctx->ch_layout = (s->nb_channels == 1)
                     ? (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO
                     : (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    avctx->sample_rate = s->sample_rate;
    avctx->frame_size  = s->lsf ? 576 : 1152;           // 1491

    frame->nb_samples = avctx->frame_size;
    ret = ff_get_buffer(avctx, frame, 0);               // 分 extended_data

    ret = mp_decode_frame(s, (OUT_INT **)frame->extended_data, buf, buf_size);
    *got_frame_ptr = 1;
    return buf_size + skipped;
}
```

#### 2.6.1 `ff_mpa_decode_header` 拆 32-bit header

`libavcodec/mpegaudiodecheader.c:34`：

```c
// 一个 MP3 header 就是 4 字节。各位拆出来：
// 31-21: sync word (11 bits, all 1)
// 20:    ID (MPEG version: 0=v2/v2.5, 1=v1)
// 19:    v2 vs v2.5 (if ID=0)
// 18-17: layer (01=Layer3, 10=Layer2, 11=Layer1)
// 16:    error protection (反转)
// 15-12: bitrate index → 查 ff_mpa_bitrate_tab[lsf][layer-1][idx]
// 11-10: sample_rate index → 查 ff_mpa_freq_tab[idx] >> (lsf + mpeg25)
// 9:     padding
// 8:     private
// 7-6:   mode (00=stereo, 01=joint, 10=dual, 11=mono)
// 5-4:   mode_extension (IS / MS stereo 标志)
// ...
```

关键派生：

```c
// MPEG-1 Layer 3: 帧大小字节数
frame_size = (bitrate_kbps * 144000) / (sample_rate << lsf) + padding;
// 1152 samples / frame (MPEG-1 Layer 3), 576 samples / frame (MPEG-2/2.5 Layer 3)
```

#### 2.6.2 每帧重建 ch_layout 的原因

MP3 比特流**允许 mono 和 stereo 帧交替出现**。罕见但规范允许。每帧重新设 `ch_layout` 是为了覆盖这种切换。这就是为什么要 `uninit` 再重设 —— 旧布局如果是 CUSTOM 会泄漏 `map` 数组（见 §2.10.1）。

#### 2.6.3 `mp_decode_frame` 的真实流水线

`libavcodec/mpegaudiodec_template.c:1212-1554`。MPEG-1 Layer 3 的核心过程：

```
input bitstream
  ↓
decode_header             // 1462: 验证 header（用 main_data_begin 字段）
  ↓
unpack side info          // 1220-1300: 读 12-bit part2_3_length,
                          //              9-bit big_values, 8-bit global_gain,
                          //              scale factor compression, block type, ...
  ↓
main_data 拼接             // 1302-1336: 上一帧尾部 + 本帧前部
                          //              (MP3 的"bit reservoir"设计)
  ↓
huffman_decode            // 1452: VLC 解 576 个量化系数到 g->sb_hybrid[576]
                          //              用 l3_unscale 做 val^(4/3) * 2^(gain/4)
  ↓
reorder_block             // 908: short block 的 3 窗口交错 → 输出顺序
  ↓
compute_stereo            // 1456: IS / MS stereo 解耦
                          //       MS: (L, R) = ((L+R)/√2, (L-R)/√2)
                          //       IS: is_tab[0][sf] / is_tab[1][sf] 乘系数
  ↓
compute_imdct             // 1132: 18-point 或 36-point IMDCT + overlap-add
  ↓
ff_mpa_synth_filter       // 1534-1552: 32-tap 多相合成滤波器 → PCM
  ↓
output frame->extended_data[ch]
```

**几个非显然的点**：

**(a) Bit reservoir**：MP3 允许一帧的 Huffman 数据"借用"**前一帧的尾部空间**存放。`side_info` 里的 `main_data_begin` 告诉解码器"回到之前 N 字节处开始读"。这就是为什么 MP3 不能做到严格的 packet-level 独立 —— 即使 demuxer 按帧切好，decoder 内部仍需保留**前一帧的字节尾巴**。`last_buf` 字段（`libavcodec/mpegaudiodec_template.c:1302`）就是这个"尾巴缓冲"。

**(b) `compute_imdct` 的 `mdct_buf` 持久状态**：

```c
// libavcodec/mpegaudiodec_template.c:89
INTFLOAT mdct_buf[MPA_MAX_CHANNELS][SBLIMIT * 18];  // 32×18 floats per channel
```

MDCT 窗口长 36 但跳步 18 —— 意味着**每次 IMDCT 的输出后 18 个样本要和下一帧的前 18 个样本叠加**。`mdct_buf` 保留上一帧的这 18 个样本，**跨 `decode_frame` 调用持续存在**。

```c
// compute_imdct (libavcodec/mpegaudiodec_template.c:1161-1208)
for (j = 0; j < 32; j++) {  // 每个 subband
    s->mpadsp.imdct36_blocks(out_ptr, win, buf, sblim, ...);
    // 内部：做 36-point IMDCT，前 18 个 + mdct_buf 叠加，后 18 个存入 mdct_buf
}
```

这就是为什么 **MP3 decoder 不是无状态的纯函数**。`avcodec_flush_buffers` 会调 `mp_flush`（`libavcodec/mpegaudiodec_template.c:1632`）清空 `mdct_buf` 和 `synth_buf`。seek 之后必须 flush，否则新位置的头两帧会因为带着旧的 overlap 状态产生咔哒声。

**(c) `ff_mpa_synth_filter`** 在 `libavcodec/mpegaudiodec_template.c:1534-1552`：

```c
for (ch = 0; ch < s->nb_channels; ch++) {
    if (s->avctx->sample_fmt == OUT_FMT_P) {             // planar
        samples_ptr  = samples[ch];
        sample_stride = 1;
    } else {                                              // interleaved
        samples_ptr  = samples[0] + ch;
        sample_stride = s->nb_channels;
    }
    for (i = 0; i < nb_frames; i++) {                     // 18 次
        ff_mpa_synth_filter(..., samples_ptr, sample_stride, s->sb_samples[ch][i]);
        samples_ptr += 32 * sample_stride;                // 每次吐 32 样本
    }
}
```

一帧 = 18 × 32 = 576 样本/声道/granule × 2 granules = **1152 样本/帧**。mp3float decoder 输出 `AV_SAMPLE_FMT_FLTP`（planar float），`frame->extended_data[0]` 是左声道 1152 个 float，`extended_data[1]` 是右声道。

#### 2.6.4 为什么 MP3 decoder 不设 `AV_CODEC_CAP_DELAY`

`libavcodec/mpegaudiodec_float.c:123-135` 的 codec 注册：

```c
.p.capabilities = AV_CODEC_CAP_CHANNEL_CONF | AV_CODEC_CAP_DR1,
// 没有 AV_CODEC_CAP_DELAY
```

**`AV_CODEC_CAP_DELAY` 表示"解码器需要提前多帧 buffer 才能出结果"**。AAC with SBR/PS 必须设 —— SBR 需要前瞻一帧才能构造完整频谱。MP3 的每帧是自包含的（除了 bit reservoir 的字节借用，但那是字节级别不是帧级别的延迟），1 packet in → 1 frame out 稳定发生。所以不设这个 flag。

面试加分：**从 `mdct_buf` 和 `bit reservoir` 看，MP3 其实是有"局部状态"的，但它的状态是上一帧留下的"遗产"，不是"前瞻"**。CAP_DELAY 语义是"要前瞻"，MP3 不需要前瞻。

---

### 2.7 `discard_samples` — 音频独有的前/后裁切

`libavcodec/decode.c:319-408`。在 `decode_simple_receive_frame` / `decode_receive_frame_internal` 里，音频路径会在 codec 出帧后**再过一道裁切**：

```c
// libavcodec/decode.c:327
side = av_frame_get_side_data(frame, AV_FRAME_DATA_SKIP_SAMPLES);
if (side && side->size >= 10) {
    avci->skip_samples = AV_RL32(side->data);          // 前裁切：encoder delay
    discard_padding    = AV_RL32(side->data + 4);      // 后裁切：end padding
}

if (avci->skip_samples >= frame->nb_samples) {
    avci->skip_samples -= frame->nb_samples;
    av_frame_unref(frame);
    return AVERROR(EAGAIN);                             // 整帧丢，要下一帧
}
if (avci->skip_samples > 0) {
    // 把 [skip_samples..nb_samples) 的数据移到 [0..nb_samples - skip_samples)
    av_samples_copy(frame->extended_data, frame->extended_data,
                    0, skip_samples,
                    frame->nb_samples - skip_samples,
                    avctx->ch_layout.nb_channels, frame->format);
    frame->nb_samples -= skip_samples;
    frame->pts        += av_rescale_q(skip_samples,    // PTS 前移
                                      (AVRational){1, avctx->sample_rate},
                                      avctx->time_base);
    avci->skip_samples = 0;
}

if (discard_padding > 0 && discard_padding <= frame->nb_samples) {
    frame->nb_samples -= discard_padding;               // 后裁直接减 nb_samples
}
```

**背景**：LAME 编 MP3 时，为了让 MDCT 窗口第一个对齐需要在最前面塞 528 或 576 个静音样本（"encoder delay"），结尾也会补齐到帧边界（"end padding"）。这些信息编到 LAME 头的 `padding_start` / `padding_end`、iTunSMPB metadata、MP4 edit list 里。**Demuxer 把信息以 side_data 形式塞给每个 packet，decoder 按帧裁**。

**为什么视频没这个问题？** 视频帧是"原子"的（每帧就是一张图），不存在"一帧里前几个像素要丢掉"的概念。音频的一帧是 PCM 样本序列，可以任意切片。

---

### 2.8 `swr_convert` — 重采样 + 格式统一

libswresample 的职责：**把 `(in_fmt, in_ch, in_rate)` 转成 `(out_fmt, out_ch, out_rate)`**。MP3 吐的是 `fltp/44100/stereo`，SDL 要的是 `s16/44100/stereo` —— 只差一个 format 转换。但 libswresample 的内部实现远比"简单转格式"复杂。

#### 2.8.1 `swr_init` 的内部工作格式选择

`libswresample/swresample.c:227-254`：

```c
// 层级规则
if (输入和输出都 ≤16-bit 且同采样率)                    int_sample_fmt = s16p;
else if (输入总位深 ≤24-bit)                           int_sample_fmt = s16p;
else if (输入 ≤2 字节 且无 rematrix 且同采样率)         int_sample_fmt = s16p;
else if (输入输出都是 s32p 且无 rematrix 且无 resample) int_sample_fmt = s32p;
else if (输入 ≤4 字节)                                 int_sample_fmt = fltp;
else                                                  int_sample_fmt = dblp;
```

**动机**：数据路径尽量走**最窄的够用格式**。整数路径（s16p/s32p）比 fltp 快 30-50%，但精度不够时必须升到浮点。对 MP3 (fltp) → s16 的场景，int_sample_fmt = s16p，**整个流水线都在 16-bit 整数域**，速度最快。

#### 2.8.2 `resample_first` 的判据

`libswresample/swresample.c:337`：

```c
s->resample_first = RSC * s->out.ch_count / s->used_ch_layout.nb_channels - RSC
                  < s->out_sample_rate / (float)s->in_sample_rate - 1.0;
// RSC = 1 (libswresample/swresample.c:304)
```

化简一下：令 `r_out = out_rate/in_rate`，`r_ch = out_channels/in_channels`：

- `resample_first = true`  ⇔  `r_ch - 1 < r_out - 1`  ⇔  `r_ch < r_out`

**直观解释**：
- **上采样 + 下混**（`r_out > 1`, `r_ch < 1`）：`r_ch < r_out` 成立 → **先 resample**（这时通道数还多，但 FFmpeg 认为 resample 的代价比 rematrix 大，先做完代价大的再下混）
- **下采样 + 上混**（`r_out < 1`, `r_ch > 1`）：条件不成立 → **先 rematrix**（先把通道数扩起来，不行 —— 等等这顺序看起来反直觉）

实际源码里的取舍比单一启发式复杂，面试时可以这样说："先 resample 还是先 rematrix 是一个代价优化问题，FFmpeg 用一个基于通道比和采样率比的启发式选顺序，具体判据在 `libswresample/swresample.c:337`。"

#### 2.8.3 五段 AudioData + 指针别名

`swr_convert_internal`（`libswresample/swresample.c:591-719`）的核心数据流：

```
in (用户传进来)
  ↓ convert_in  (输入格式 → int_sample_fmt)
postin
  ↓ resample 或 rematrix (看 resample_first)
midbuf
  ↓ 另一个
preout
  ↓ convert_out (int_sample_fmt → 输出格式)
out (用户的输出缓冲)
```

**指针别名优化**（libswresample/swresample.c:627-647）：

```c
if (s->int_sample_fmt == s->in_sample_fmt && s->in.planar && !s->channel_map)
    postin = in;                                        // 别名，不分配不拷贝
// output 侧同理
```

**动机**：音频处理是 memory-bandwidth-bound，每多一次 memcpy 都是性能损失。如果输入格式已经是内部工作格式（比如 MP3 decoder 输出 fltp、int_sample_fmt 也是 fltp），`postin` 直接指到 `in` 的缓冲，convert_in 阶段完全跳过。

**对 MP3 → s16 场景的具体意义**：in=fltp, int=s16p —— 第一次 convert 不能省。out=s16, int=s16p —— 第二次 convert 不能省（planar → packed）。这条路径做了两次真实转换 + 一次数据流过 rematrix（对 stereo→stereo 是单位矩阵，优化路径会跳过）+ 不做 resample。

#### 2.8.4 polyphase FIR 重采样核心

`libswresample/resample.c:184-278`。重采样器是 **Kaiser-windowed sinc 的多相 FIR 滤波器**：

```c
// libswresample/resample.c:41-131: build_filter
// 1. 计算截止频率 factor = min(out_rate * cutoff / in_rate, 1.0)
// 2. filter_length = ceil(filter_size / factor)  // 默认 filter_size=32
// 3. phase_count = 1 << phase_shift              // 默认 phase_shift=10, 即 1024 相
// 4. 每个相位 i / phase_count：生成 filter_length 个 sinc 系数
//    y[k] = sinc(k - filter_length/2 + i/phase_count) * kaiser_window(k, beta)
// 5. 量化存入 filter_bank
//    - int_sample_fmt == s16p: int16, shift=15
//    - int_sample_fmt == s32p: int32, shift=30
//    - int_sample_fmt == fltp: float, shift=0
```

**每次 `swr_convert` 的推进逻辑**：

```c
// 内部状态
int64_t index;    // 整数部分：下一次卷积的起点样本索引（相对 in_buffer 头）
int32_t frac;     // 小数部分：src_incr 精度
int32_t src_incr; // 等效 in_rate
int32_t dst_incr; // 等效 out_rate
// 每出一个样本：index += src_incr / dst_incr，frac += src_incr % dst_incr，进位时 index++
```

**延迟缓冲**：`in_buffer_count - (filter_length-1)/2` 是"已经在 buffer 里但因为 filter center 还没覆盖它所以暂时出不来"的样本数。下一次有新输入进来才能继续推进。

#### 2.8.5 `swr_get_delay` 的公式

`libswresample/resample.c:408-416`：

```c
static int64_t get_delay(struct SwrContext *s, int64_t base) {
    ResampleContext *c = s->resample;
    int64_t num = s->in_buffer_count - (c->filter_length - 1) / 2;
    num *= c->phase_count;
    num -= c->index;
    num *= c->src_incr;
    num -= c->frac;
    return av_rescale(num, base, s->in_sample_rate * (int64_t)c->src_incr * c->phase_count);
}
```

**返回值单位**：`base` 时间基下的样本数。常见用法 `swr_get_delay(ctx, in_rate)` 返回"输入采样率下还剩几个样本没流出来"。audio_flow 里用它计算下次 `av_samples_alloc` 的大小：

```c
int64_t delay = swr_get_delay(swr_ctx, dec_ctx->sample_rate);
int out_samples = av_rescale_rnd(delay + frame->nb_samples,
                                 OUT_SAMPLE_RATE, dec_ctx->sample_rate,
                                 AV_ROUND_UP);
```

#### 2.8.6 Flush 的 reflection padding

`libswresample/swresample.c:763-768` + `libswresample/resample.c:437-454`：

```c
// 用户 swr_convert(ctx, out, N, NULL, 0) 表示"没有更多输入了"
static int resample_flush(SwrContext *s) {
    int reflection = (FFMIN(s->in_buffer_count, c->filter_length) + 1) / 2;
    for (i = 0; i < a->ch_count; i++) {
        for (j = 0; j < reflection; j++) {
            // 对 in_buffer 末尾做镜像填充
            memcpy(a->ch[i] + (index+count+j)*bps,
                   a->ch[i] + (index+count-j-1)*bps, bps);
        }
    }
    s->in_buffer_count += reflection;                   // 当作"有新输入"
}
```

**为什么要镜像？** 简单用零填充会在波形末尾产生陡峭下降，FIR 卷出来是明显的卡嗒声。镜像填充（`x[N-1], x[N-2], ...`）保持波形"局部对称"，滤波器输出平滑过渡到静音。**这是数字信号处理领域的标准技巧**。

如果 audio_flow/main.c 在结尾**没调** `swr_convert(..., NULL, 0)` flush（它确实没调），就会少几毫秒的尾音 + 可能有一点点 artifact。教学代码可以接受，生产代码必须做。

#### 2.8.7 rematrix 系数

`libswresample/rematrix.c:347-451` 的 `swr_build_matrix2` 构造混音矩阵：

```c
// 典型 5.1 → stereo (默认规则)
matrix[FL][FL]   = 1.0;
matrix[FL][FC]   = M_SQRT1_2;                          // 中置 /√2 分到 L/R
matrix[FL][BL]   = M_SQRT1_2 * surround_mix_level;     // 后 L → 前 L，按 surround 电平
matrix[FL][LFE]  = lfe_mix_level * M_SQRT1_2;          // 低音 /√2 分到 L/R

// Dolby Surround / DPL2 特殊混合（libswresample/rematrix.c:221-224）
// 后声道相位编码到前声道，使得 DPL 解码器能还原出后声道
```

`M_SQRT1_2 ≈ 0.707` = 1/√2，保证功率不变（两路相同信号混合时功率加 2 倍，幅度 /√2 抵消）。LFE 默认走这个混合，可以通过 option 关闭。

---

### 2.9 `SDL_QueueAudio` — 送扬声器

FFmpeg 职责到 `swr_convert` 结束。`audio_flow/main.c` 用 SDL2 做输出，push 模式：

```c
// 设备 open 时 want.callback = NULL
SDL_QueueAudio(audio_dev, out_buf, queue_bytes);
// 背压控制
while (SDL_GetQueuedAudioSize(audio_dev) > 1024 * 1024)
    SDL_Delay(10);
```

**push 模式 vs callback 模式**：
- **Callback**：SDL 内部音频线程定时调用户的 callback 拉数据。低延迟，但 callback 必须实时（不能阻塞 I/O、不能 malloc），需要用 ring buffer + 原子/锁跨线程传数据。代码量大。
- **Push (`SDL_QueueAudio`)**：应用层主动推，SDL 内部维护队列，自己拉。代码简单，但延迟 = 队列大小。audio_flow 用这个是学习示例的取舍。

**背压机制的必要性**：解码速度远快于播放速度（4KB MP3 → 1152 samples × 4 bytes × 2 ch = 9KB PCM，解一帧只要几毫秒，播一帧要 26ms）。不做背压队列会无限膨胀。这里用 `SDL_Delay(10)` 粗粒度阻塞生产者，等队列降下来再继续 —— 简单粗暴但对教学代码够用。

---

### 2.10 横切基础设施

前面流程里反复出现的几个共用机制，单独讲。

#### 2.10.1 AVChannelLayout 的 5.1 API 重构

旧 API（FFmpeg ≤ 5.0）：两个字段 `int channels; uint64_t channel_layout;`。问题：
- 64 位掩码最多 64 个通道，Ambisonic（球谐系数可达 16/64/256 阶）描述不了
- `channels == 8` 可能是 7.1，也可能是 CUSTOM 乱排
- 两字段容易不一致

新 API（`libavutil/channel_layout.h`）：

```c
typedef struct AVChannelLayout {
    enum AVChannelOrder order;       // NATIVE/CUSTOM/AMBISONIC/UNSPEC
    int nb_channels;
    union {
        uint64_t mask;               // NATIVE / AMBISONIC
        AVChannelCustom *map;        // CUSTOM，堆分配
    } u;
    void *opaque;
} AVChannelLayout;
```

四种 `order`：

| Order | 含义 | 典型 |
|---|---|---|
| `NATIVE` | 位掩码，通道顺序固定 | mono/stereo/5.1 |
| `CUSTOM` | `map` 数组显式指定 | 通道重排 |
| `AMBISONIC` | 球谐展开 | VR/360° |
| `UNSPEC` | 只知道通道数 | 未知来源 |

**`av_channel_layout_uninit` 的陷阱**（`libavutil/channel_layout.c:442`）：

```c
void av_channel_layout_uninit(AVChannelLayout *ch) {
    if (ch->order == AV_CHANNEL_ORDER_CUSTOM)
        av_freep(&ch->u.map);                          // 只 CUSTOM 释放
    memset(ch, 0, sizeof(*ch));
}
```

**只有 CUSTOM 有实际动作**。NATIVE 静态字面量（`AV_CHANNEL_LAYOUT_STEREO`）不调 uninit 也不泄漏；但 CUSTOM 漏调 uninit 就泄漏 `map` 数组。audio_flow/main.c 直接用 `AV_CHANNEL_LAYOUT_STEREO` 静态字面量就是为了规避这个坑（见 §4 Bug A）。

#### 2.10.2 `data[]` vs `extended_data`

```c
// libavutil/frame.h
uint8_t *data[AV_NUM_DATA_POINTERS];                   // AV_NUM_DATA_POINTERS = 8
uint8_t **extended_data;
```

规则（`libavutil/frame.c:146-202` 的 `get_audio_buffer`）：
- 通道数 ≤ 8：`extended_data = &frame->data[0]`，两者指同一个位置
- 通道数 > 8：`extended_data` 指向**堆**上新分配的指针数组

**为什么这么别扭？** 历史包袱。视频代码诞生时 `data[0..7]` 够用（Y/U/V + alpha + NV12 的多 plane），音频 Ambisonic 后来要求 16/64 通道。FFmpeg 不想 break 视频 API，就在音频路径引入 `extended_data` 作为"标准入口"。**音频代码永远用 `extended_data`**，这样 ≤8 和 >8 通道一视同仁。

#### 2.10.3 `AVBufferRef` 引用计数

```c
struct AVBuffer {
    uint8_t *data;
    int size;
    atomic_uint refcount;
    void (*free)(void *opaque, uint8_t *data);
    void *opaque;
};
struct AVBufferRef {
    AVBuffer *buffer;
    uint8_t *data;                                     // buffer->data 的别名，可能偏移
    size_t size;
};
```

`av_buffer_ref` 返回新 `AVBufferRef` 指向同一 `AVBuffer`，原子 +1。`av_buffer_unref` 原子 -1，到 0 才调 `free`。

**`av_frame_unref` 干的事**：对 `frame->buf[0..N]` 和 `frame->extended_buf[0..N]` 每个 ref 都 unref，然后整个 struct 清零（但不释放 struct 本身 —— `av_frame_free` 才释放 struct）。

**这设计让 zero-copy 成为可能**：decoder 把解出的 frame 给 filter，filter 给另一个 filter，最终给 encoder，中间全程不 memcpy —— 每一级只是 `av_frame_ref`（refcount +1）。MP3 的 1152×4×2 字节不算什么，但 4K 视频 (3840×2160×3 = 24MB / 帧) 必须 zero-copy，不然 CPU 没法做别的。

#### 2.10.4 时间戳与时间基

```c
AVPacket::pts, dts                  // container 时间，单位 AVStream::time_base
AVFrame::pts                         // decoder 从 packet 继承
AVFrame::pkt_dts                     // 对应 packet 的 dts
AVFrame::best_effort_timestamp       // 启发式修正后的值
```

**两个时间基容易搞混**：
- `AVStream::time_base`：demuxer 决定。MP3 常见 `1/14112000`，H.264 in MP4 常见 `1/90000` 或 `1/timescale`
- `AVCodecContext::time_base`：**encoder 用**（设置编码帧率/时间基），**decoder 不用设**

换算：`av_packet_rescale_ts(pkt, in_tb, out_tb)` 或手动 `av_rescale_q`。

**`guess_correct_pts`**（`libavcodec/decode.c:293-317`）：

```c
dc->pts_correction_num_faulty_dts += dts <= last_dts;
dc->pts_correction_num_faulty_pts += reordered_pts <= last_pts;
// 谁违反单调性次数少，就信谁
return pts_faulty <= dts_faulty ? reordered_pts : dts;
```

**统计启发**，不是严格算法。视频 B-frame 重排时 dts < pts 是正常的，PTS 才是"播放时间"，DTS 是"解码时间"；音频无 B-frame，两者相等。

#### 2.10.5 线程模型

| 对象 | 线程安全？ |
|---|---|
| `AVFormatContext` | **否**。单实例单线程 |
| `AVCodecContext` | **否**。单实例单线程调 send/receive |
| `av_log` | 是（内部有锁） |
| `AVFrame` / `AVPacket` | 数据 refcounted 可跨线程共享，但单实例不能并发改写 |

**想多线程解码**：
- **每个线程一个独立的 `AVCodecContext`**（常见做法，比如并行转码不同文件）
- `AV_CODEC_CAP_FRAME_THREADS` / `AV_CODEC_CAP_SLICE_THREADS` 让**单个 decoder 内部开线程** —— 但 **只对视频有效**，音频 decoder 没实现（一帧本来就小，线程切换开销大于收益）

`avformat_find_stream_info` 强制 `threads=1` 的原因**仅针对 H.264 SPS/PPS 提取问题**（见 §2.2），不是音频的限制。

---

## 3. 50 道面试问答

### 架构（Q1-5）

**Q1. FFmpeg 的四个核心 lib 分别负责什么？**
- `libavformat`：解封装/封装（demuxer/muxer）+ 传输协议（http/rtmp/rtp）
- `libavcodec`：编解码器（~400 个）
- `libavutil`：公共基础（内存、数学、日志、像素格式、通道布局、AVFrame/AVPacket 结构）
- `libswresample`：音频重采样/格式转换/通道重映射
- `libswscale`：视频图像缩放/像素格式转换
- `libavfilter`：滤镜图

**Q2. AVFormatContext 和 AVCodecContext 的关系？**
`AVFormatContext` 拿的是"这个文件/流里有哪些流、每个流的时间基和 codec_parameters"。`AVCodecContext` 是实际的解码器状态机。两者通过 `avcodec_parameters_to_context` 对接——codec_parameters 在 stream 里，告诉你该开哪种 decoder。

**Q3. 为什么 FFmpeg 把 demux 和 decode 彻底分离？**
解耦传输层和编解码层。同一个 mp3 decoder 可以解来自 http、file、pipe 的数据；同一个 mp3 demuxer 可以输出给 mp3float 或 mp3 decoder。面试加分：这让 **remuxing**（改封装不改编码，如 `ffmpeg -i a.mkv -c copy a.mp4`）只走 demux→mux，不走 decode→encode。

**Q4. AVIOContext 的价值是什么？**
把一切 I/O 抽象成 `read/write/seek` 三个回调。file、http、memory buffer、pipe 甚至自定义的分块下载都能包成 AVIOContext，demuxer 完全不知道数据来源。这是 FFmpeg 能接任意协议的基础。

**Q5. 为什么 FFmpeg 要把旧的 `avcodec_decode_audio4`/`avcodec_decode_video2` 替换成 `send_packet/receive_frame`？**
旧 API 是 pull 模型：一次 decode 返回 `got_frame_ptr` 和"这个 packet 里还剩多少字节"。问题：无法表达"一个 packet 解出多个 frame"（如 AAC HE-AACv2 SBR/PS）和"多个 packet 拼一个 frame"。新 API 的 push 模型把"喂数据"和"取结果"完全解耦，天然支持 1:N 和 N:1。

### Demuxer 层（Q6-13）

**Q6. `av_read_frame` 内部有几级缓冲，各自作用？**
- **raw_packet_buffer**（在 `ff_read_packet` 里）：流探测阶段的 packet 缓冲，用于 probesize 检查和 codec 参数推断
- **packet_buffer**（在 `av_read_frame` 里）：`AVFMT_FLAG_GENPTS` 模式下为推算缺失 PTS 做的乱序缓冲
普通音频只走第一级。

**Q7. `FFERROR_REDO` 是什么？**
内部错误码（不对外暴露）。demuxer 返回它表示"我消化了这段数据用来做 probing，但不要作为 packet 返回给调用者，请再 read 一次"。避免向上层暴露用于探测的半成品数据。

**Q8. 为什么 `avformat_find_stream_info` 要强制 `threads = 1`？**
多线程解码会导致 H.264 的 SPS/PPS 提取不可靠。这是历史限制。音频本来就不多线程，这个设置对音频没影响但会拖慢视频探测。

**Q9. MP3 demuxer 如何识别"真"的 MP3 起点？**
光匹配一个 sync 字 `0xFFFB` 不够（ID3 tag、图片数据里太容易碰撞）。`mp3_read_probe` 的做法：匹配 sync → 按 `h.frame_size` 跳到下一个位置 → 再匹配 → 连续 N 次有效才认为是真 MP3。`header_emu` 计数器防止假阳性。

**Q10. Xing/VBRI 头对 MP3 有什么用？**
MP3 本身无全局元信息。VBR（Variable Bitrate）情况下，不读完整文件没法知道总时长。Xing/VBRI 头放在第一个 MP3 frame 的 padding 区，记录总帧数、总字节数、TOC（seek 索引）。没有它，demuxer 的 duration 和 seek 都是估算。

**Q11. `AVFMT_FLAG_GENPTS` 的工作原理？**
对某些 demuxer（早期的 AVI、TS），PTS 可能缺失或 B-frame 乱序。GENPTS 打开后，`av_read_frame` 会**缓冲多个 packet**，等能从 DTS 推算出正确的 PTS 顺序，再一个一个 pop 出来。对音频（无 B-frame）基本没用。

**Q12. HTTP demuxer 是怎么知道流是否可 seek？**
发送 `HEAD` 请求看 `Accept-Ranges: bytes`，或者尝试 `Range: bytes=...` 请求看是否返回 `206 Partial Content`。探测到就把 `AVIOContext::seekable = AVIO_SEEKABLE_NORMAL`。上层 `avformat_seek_file` 看这个标志决定能否 seek。

**Q13. `probesize` 和 `max_analyze_duration` 分别控制什么？**
- `probesize`：探测阶段**最多读多少字节**。太小会识别不出 codec，太大会启动慢。
- `max_analyze_duration`：最多读**多少时长**（us）的数据。MP3 默认 5 秒。
- 两者是或的关系，任一满足就停。

### Decoder 层（Q14-23）

**Q14. 解释 `send_packet`/`receive_frame` 的 EAGAIN 语义。**
| 函数 | EAGAIN 含义 | 你该做什么 |
|---|---|---|
| `send_packet` | 上一个 packet 还没被消化 | 先 `receive_frame` 把 output buffer 倒空 |
| `receive_frame` | output 空，需要更多输入 | 先 `send_packet` |

规则：**同一个解码器实例不可能两个函数同时返回 EAGAIN**（否则死锁）。

**Q15. 为什么 `send_packet` 会主动调一次 `receive_frame_internal`？**
优化：避免"send→return→receive→return"两次系统调用。如果 output buffer 是空的，send 完立刻尝试拉一帧填进去。这样常见循环（send 一个 packet，立刻 receive 一个 frame）更顺滑。

**Q16. flush/drain 机制怎么工作？**
调 `avcodec_send_packet(ctx, NULL)` 告诉 decoder"没有更多输入了"，内部 `draining_started = 1`。之后只能 `receive_frame` 不能 `send_packet`（会返 EOF）。`receive_frame` 继续返回已经缓冲的帧，直到返回 `AVERROR_EOF` 表示彻底排空。

**Q17. `AV_CODEC_CAP_DELAY` 对音频意味着什么？**
这个标志表示"解码器有跨帧延迟，需要 flush 才能拿到最后几帧"。视频里 H.264 B-frame 重排就需要这个。**MP3 没有**，1 packet → 1 frame。但 AAC LC 的 SBR/PS 工具会引入延迟，AAC decoder 有 `AV_CODEC_CAP_DELAY`。

**Q18. 为什么 MP3 帧恒定是 1152 samples？**
MPEG-1/2 Layer 3 规范规定：一帧包含 2 个 granules，每个 granule 18 subband × 32 samples = 576 samples，总计 1152。这是 MDCT 变换的固定窗口大小。MP3 的比特率变化靠 frame size（字节数）变，不靠 sample 数变。

**Q19. MDCT + overlap-add 是什么，为什么解码器需要维护隐藏状态？**
MDCT（修正离散余弦变换）是一种 lapped transform：相邻帧在时域上有 50% overlap。解码时 IMDCT 出来的数据要和**前一帧的后半部分叠加**才是最终 PCM。这意味着：
- 解码器必须保存前一帧的 IMDCT 残留（`mdct_buf` 循环缓冲）
- 开始解码时前半帧是"冷启动"的，通常会输出静音或前一帧残留
- 这就是为什么 MP3 编码有 "encoder delay"：编码器必须在开头塞静音让解码器有正确的起始状态

**Q20. `discard_samples` 是什么？为什么视频没这个？**
音频编码器（LAME、FAAC）为了对齐 MDCT 窗口会在开头塞几百个静音样本（encoder delay），结尾也会塞 padding。这些信息写在 LAME 头、iTunSMPB 或 MP4 Edit List 里。`discard_samples` 读取这些元信息，在解码输出前把前后的无效样本裁掉。视频每一帧都是独立的显示单元，没这个问题。

**Q21. `mp3float` 和 `mp3`（fixed-point）decoder 的区别？**
两套实现共享 `mpegaudiodec_template.c`，只是类型参数不同：
- `mp3float`：内部用 float 计算，精度高，现代 CPU 首选
- `mp3`（定点）：用 32 位定点，无 FPU 的嵌入式平台首选
- 输出样本格式前者是 `fltp`，后者是 `s16p`
`avcodec_find_decoder(AV_CODEC_ID_MP3)` 默认返回 `mp3float`。

**Q22. 音频 decoder 的线程模型是什么？**
音频 decoder **默认不启用线程**。`FF_THREAD_FRAME`（帧并行）对音频无意义——MP3/AAC 一帧才 20ms，并行收益为零，反而增加延迟。`FF_THREAD_SLICE` 对音频也没用，一帧没法切片。**音频加速靠 SIMD（AVX/NEON），不靠多线程。**

**Q23. `best_effort_timestamp` 是怎么算出来的？**
`libavcodec/decode.c:293` 的 `guess_correct_pts` 启发式：统计 PTS 和 DTS 分别违反单调性的次数，谁违反得少就信谁。不是严格算法，而是对不可靠 demuxer 的兜底。

### 声道布局（Q24-28）

**Q24. 为什么 FFmpeg 5.1 重构声道 API？**
旧 API `int channels + uint64_t channel_layout`：
- 64 位掩码不够表达 Ambisonic / 16+ 通道影院系统
- 两个字段容易不一致
- 无法区分"8 通道 7.1"和"8 通道 custom 布局"

新 API `AVChannelLayout` 加 `order` 字段分成 NATIVE/CUSTOM/AMBISONIC/UNSPEC 四种语义，并用 union 区分位掩码和显式 map 数组。

**Q25. `AV_CHANNEL_ORDER_UNSPEC` 和 `NATIVE` 有什么区别？**
- `NATIVE`：知道每个通道是什么（FL, FR, FC, LFE...），可以做 rematrix
- `UNSPEC`：只知道有 N 个通道，不知道含义。swresample 遇到 UNSPEC 会默认按通道数填一个 NATIVE 布局，但这是猜测

**Q26. `av_channel_layout_uninit` 什么时候真正释放内存？**
只有 `order == CUSTOM` 时才会 `av_freep(&ch_layout->u.map)`。NATIVE 本质是栈上数据，`uninit` 只是 memset 清零。**这就是为什么 `AV_CHANNEL_LAYOUT_STEREO` 这种静态字面量不需要配对 `uninit`。**

**Q27. `av_channel_layout_copy` 和 `=` 赋值的区别？**
对 NATIVE 而言两者等价。对 CUSTOM：`=` 只复制指针，两个变量共享 `map` 数组，一个 `uninit` 另一个就变野指针。`av_channel_layout_copy` 对 CUSTOM 会 `av_malloc_array` 新的 map 并 memcpy。**Always use copy for struct members.**

**Q28. 为什么 `AV_CHANNEL_LAYOUT_STEREO` 比 `av_channel_layout_default(&l, 2)` 更推荐？**
1. 字面量是编译期常量，没有函数调用开销
2. 不需要配对 `uninit`
3. 错误路径上不可能泄漏（default 之后如果 `swr_init` 失败，你必须记得 uninit）
4. 代码更清晰

`study/audio_flow/main.c` 就是因为 review 发现这个坑才从 `default+uninit` 改成静态字面量的。

### Frame 内存模型（Q29-33）

**Q29. `AVFrame::data[]` 和 `extended_data` 什么关系？**
`data` 是 `uint8_t *data[8]` 固定 8 指针数组。`extended_data` 是 `uint8_t **`。
- 通道数 ≤ 8：`extended_data = &data[0]`，共用
- 通道数 > 8：`extended_data` 指向堆上分配的指针数组

**音频代码永远用 `extended_data`，这样 ≤8 和 >8 通道是同一套代码。**

**Q30. Planar 和 Packed 音频格式的区别？**
- **Packed**（`s16`, `fltp` → 不，`flt` 才是 packed）：`LRLRLRLR...` 连续交错，只占 `data[0]` 一个平面
- **Planar**（`fltp`, `s16p`）：`LLLL... RRRR...` 每个通道一个 plane，占 `data[0..N-1]`

SDL 需要 packed S16，但 MP3 解出来是 planar float，所以必须 swresample 转换。

**Q31. `AVBufferRef` 如何实现引用计数？**
```c
struct AVBuffer { ... int refcount; ... };
struct AVBufferRef { AVBuffer *buffer; uint8_t *data; int size; };
```
`av_buffer_ref(ref)` 创建新 ref 并 `atomic_fetch_add(&buffer->refcount, 1)`。`av_buffer_unref(ref)` 减一到 0 时调 `buffer->free` 回调。这使得同一块 PCM 数据可以被 AVFrame、filter、显示队列共享。

**Q32. `av_frame_unref` 和 `av_frame_free` 的区别？**
- `av_frame_unref(frame)`：对每个 `buf[i]` 和 `extended_buf[i]` 调 `av_buffer_unref`，然后 `memset` frame 本身。frame 结构仍然活着，可以再用。
- `av_frame_free(&frame)`：先 unref，再 `av_freep(frame)`。frame 指针变 NULL。

循环解码的正确模式：循环开头 `av_frame_unref(frame)`；只在结束时 `av_frame_free(&frame)`。

**Q33. `av_samples_alloc` 的内存布局是什么？**
**一次 malloc，内部切片。** 对 planar 格式：
- `size = linesize × planes`（linesize 是对齐后的每 plane 字节数）
- `audio_data[0] = buf`, `audio_data[1] = buf + linesize`, ...

单块分配的好处：cache 友好、只一次 malloc 开销。坏处：任何一个 plane 都不能单独释放。

### Swresample（Q34-41）

**Q34. `swr_init` 构建的流水线有哪几级？**
1. 输入格式转换 → 内部工作格式
2. 第一次 rematrix（如果 `resample_first` 为 false）
3. 重采样（polyphase 滤波器）
4. 第二次 rematrix（如果 `resample_first` 为 true）
5. 内部格式 → 输出格式

级数动态剪枝：格式相同时用指针别名跳过对应阶段。

**Q35. `resample_first` 控制什么？**
决定"重采样"放在"声道混音"之前还是之后。启发式选择：
- 上采样 + 下混 → 先下混（少量通道再重采样，省 CPU）
- 下采样 + 上混 → 先下采样（减少总样本数再 rematrix）

**Q36. 为什么 swr_convert 要用"指针别名"优化？**
音频处理瓶颈是内存带宽不是 CPU 运算。每多一次 memcpy 就多一次 cache miss。别名让"直通路径"（输入/输出格式已经等于内部格式）零拷贝。

**Q37. `swr_get_delay` 返回什么？怎么用？**
返回**重采样器内部延迟缓冲里当前有多少样本**，单位换算到你指定的 base 时间基。两个用途：
1. 算输出缓冲大小：`av_rescale_rnd(delay + in_samples, out_rate, in_rate, AV_ROUND_UP)`
2. 调整输出 pts（下游知道实际的显示时刻）

**Q38. `swr_convert(ctx, out, N, NULL, 0)` 是 flush 信号，为什么？**
传 `in_arg == NULL` 告诉 swresample"没有更多输入了，把你内部延迟缓冲的数据吐出来"。返回的 `converted` 可能小于 N，也可能是 0（之前已经 flush 过）。**面试加分**：`study/audio_flow` 的 flush 路径故意没走 swr_convert，导致末尾几毫秒被丢——学习版有意简化。

**Q39. 什么时候需要 rematrix？**
输入输出声道布局不同时：
- 上混（mono → stereo）：L = R = mono
- 下混（5.1 → stereo）：按 ITU-R BS.775 公式 `L = FL + FC/√2 + BL/√2` 等
- 通道重排（SMPTE → FFmpeg order）
默认矩阵由 `build_matrix` 根据输入输出 layout 自动生成。

**Q40. SOXR 和内置 polyphase resampler 有何区别？**
- **内置**（`swri_resampler`）：Kaiser-windowed sinc 的多相滤波器，可调 `cutoff/kaiser_beta/phase_count`。默认质量足够，速度快。
- **SOXR**（`swri_soxr_resampler`）：外部库 libsoxr，精度更高（VHQ 档可以到 -150 dB 噪声），但慢 2-3 倍。
通过 `-af "aresample=resampler=soxr:precision=28"` 选择。

**Q41. 44.1 kHz → 48 kHz 有理论精度代价吗？**
有。比率 48/44.1 = 160/147 不是整数倍，需要**有理数重采样**（上采 160 倍、滤波、下采 147 倍）。理论上完美 sinc 滤波器无损，但实际 FIR 滤波器长度有限，会引入微弱 alias 和 passband ripple。典型商用 resampler 能做到 -100 dB 以下。

### 时间戳和线程（Q42-46）

**Q42. `pts`、`dts`、`time_base` 的关系？**
- `pts` / `dts`：整数，单位是 `time_base`（一个有理数）
- 实际秒 = `pts × time_base.num / time_base.den`
- 比如 `time_base = {1, 14112000}`, `pts = 14112000` 就是 1 秒

为什么不直接用 double？精度 + 跨平台一致性。

**Q43. `AVStream::time_base` 和 `AVCodecContext::time_base` 的区别？**
- `AVStream::time_base`：demuxer 给的 container 时间基。读进来的 packet pts 是这个单位。
- `AVCodecContext::time_base`：encoder 时用来告诉 encoder "我送的 frame 的 pts 是这个单位"。**decoder 不读它，只读 AVStream 的。**

混淆这两个是大坑。`av_packet_rescale_ts(pkt, in_tb, out_tb)` 负责换算。

**Q44. `guess_correct_pts` 启发式是怎么回事？**
看代码：对每帧统计 "pts ≤ 上一个 pts" 和 "dts ≤ 上一个 dts" 的次数。谁违反单调性次数少，就信谁。这不是严格算法，是对 demuxer 不可靠的兜底。视频 B-frame 重排时 dts < pts 是正常的，这个启发式不完美但工程上够用。

**Q45. 为什么 AVFormatContext 线程不安全？**
- 内部有 packet_buffer 链表，不加锁
- 流探测阶段会修改 stream 数组
- seek 会重置所有 stream 的内部状态

要多线程读？每个线程开独立 AVFormatContext。或者用单线程 reader + packet queue。

**Q46. `av_compare_mod` 和 `pts_wrap_bits` 是什么？**
某些 container（MPEG-TS）的 PTS 是 33 位循环计数器，到 `2^33` 会翻回 0。`pts_wrap_bits = 33` 表示这个流的时间戳在 33 位处翻转。`av_compare_mod(a, b, mod)` 比较模 `2^bits` 下哪个大——类似 TCP sequence number 的环绕比较。

### 错误处理和 API 约定（Q47-50）

**Q47. EAGAIN 的"方向性"面试题怎么答？**
见 Q14。核心：send 的 EAGAIN 和 receive 的 EAGAIN 方向相反，同一个错误码有对立含义。

**Q48. `AVERROR_INVALIDDATA` 什么时候出现？**
比特流不符合规范。例如：
- MP3 frame sync 字不对
- AAC ADTS header 的 profile 字段非法
- H.264 NAL unit 长度超过 NALU 大小
通常**不是致命错误**，上层可以跳过这个 packet 继续读下一个。

**Q49. 为什么 `avformat_open_input` 要求 `fmt_ctx` 参数为 NULL？**
签名是 `int avformat_open_input(AVFormatContext **ps, ...)`：
- 如果 `*ps == NULL`，函数内部 `avformat_alloc_context` 分配一个
- 如果 `*ps != NULL`，函数会用你预先 alloc 好的 context（用于设置 options、IO callback）

所以首次调用一般初始化为 NULL：`AVFormatContext *fmt_ctx = NULL; avformat_open_input(&fmt_ctx, ...)`。

**Q50. `av_strerror` 和 `AV_ERROR_MAX_STRING_SIZE` 有什么用？**
FFmpeg 的 AVERROR 是自己定义的整数（通常是负的 `errno` 或自定义值）。直接 `printf("%d", ret)` 没意义。用：
```c
char errbuf[AV_ERROR_MAX_STRING_SIZE];  // = 64
av_strerror(ret, errbuf, sizeof(errbuf));
fprintf(stderr, "error: %s\n", errbuf);
```
或宏 `av_err2str(ret)` —— 但这个宏返回临时数组指针，**不能跨函数传递**（悬挂指针）。

---

## 4. 使用 FFmpeg 的 5 个坑（STAR 格式）

### Bug A: `av_channel_layout_default + uninit` 配对在错误路径上泄漏

**Situation**
在 `study/audio_flow/` 里配置 SwrContext，输出布局用 `av_channel_layout_default(&out_ch_layout, 2)` 初始化一个栈变量，然后 `swr_alloc_set_opts2` 传入，`swr_init`，最后 `av_channel_layout_uninit`。

**Task**
代码 review 阶段，quality reviewer 指出这是 **Important** 级别的错误路径缺陷。

**Action**
分析发现问题：`uninit` 放在 `swr_init` 后面，如果 `swr_init` 返回负值走 `goto cleanup`，`out_ch_layout` 就不会被 `uninit`。但因为这里是 NATIVE 布局（`default` 填的 stereo），实际上 `uninit` 是空操作，所以没真的泄漏——只是**代码健壮性依赖对 `default` 内部行为的假设**。如果别的路径换成 CUSTOM layout，这个模式就会漏。

解决方案：用 `AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO;` 字面量初始化，彻底去掉 init/uninit 这对配对。提交 `44ff8af`。

**Result**
- 代码更短更清晰
- 任何错误路径都不可能泄漏（静态字面量没什么可释放的）
- Re-review 通过
- **核心教训**：FFmpeg 的 "init + uninit 成对"API 在错误路径上容易漏。能用静态字面量就别用函数初始化。

---

### Bug B: `printf("%s", av_get_sample_fmt_name(fmt))` 未定义行为

**Situation**
`study/audio_flow/` Task 3 打印解码器信息：
```c
printf("  样本格式: %s\n", av_get_sample_fmt_name(dec_ctx->sample_fmt));
```
能跑，输出 `fltp`，看起来没问题。

**Task**
Quality reviewer 指出：`av_get_sample_fmt_name` 对非法或未知 `sample_fmt` 会返回 `NULL`，`printf("%s", NULL)` 是 C 标准里的 **undefined behavior**——glibc 会打印 `(null)` 但 macOS libSystem 和 musl 直接 segfault。

**Action**
用三元运算守卫：
```c
const char *sfmt_name = av_get_sample_fmt_name(dec_ctx->sample_fmt);
printf("  样本格式: %s\n", sfmt_name ? sfmt_name : "unknown");
```
同时发现 `decoder->long_name` 也可能是 NULL——FFmpeg `--enable-small` 编译配置会把所有 `.long_name` 字段置空。提交 `00e7626` 一并修复。

**Result**
- 代码在任何 FFmpeg 配置、任何 libc 实现下都安全
- **核心教训**：FFmpeg 的很多"拿字符串名"的函数（`av_get_sample_fmt_name`、`av_get_pix_fmt_name`、`av_fourcc_make_string`）对非法输入返回 NULL，永远要守卫。`AVCodec::long_name`、`AVFormat::long_name` 在 small build 下是 NULL。

---

### Bug C: 解码结束不调 flush 导致尾部音频丢失

**Situation**
一个简单的 mp3 转 wav 工具：循环 `av_read_frame` → `send_packet` → `receive_frame` → 写 PCM。测试发现输出 wav 比输入 mp3 短了 50~100 ms，末尾被截断。

**Task**
定位尾部丢失的源头。

**Action**
检查发现两处都没 flush：
1. `avcodec_send_packet(dec_ctx, NULL)` 没调，decoder 内部缓冲的最后几帧（AAC decoder 有 `AV_CODEC_CAP_DELAY`）根本没取出
2. `swr_convert(swr_ctx, out, n, NULL, 0)` 没调，重采样器的多相滤波器延迟缓冲里还有几十个样本没吐出

修复：循环结束后加两段：
```c
// Flush decoder
avcodec_send_packet(dec_ctx, NULL);
while (avcodec_receive_frame(dec_ctx, frame) == 0) {
    // ... 走完整个 swr_convert + 写 PCM 流程 ...
}
// Flush swresample
while (1) {
    int converted = swr_convert(swr_ctx, &out_buf, out_samples, NULL, 0);
    if (converted <= 0) break;
    write_pcm(out_buf, converted * bytes_per_sample);
}
```

**Result**
- 输出时长误差 <1 ms（只剩下编码器 encoder_delay 无法完全补偿）
- **核心教训**：FFmpeg 的 decoder 和 swresample 都有**内部延迟缓冲**，send NULL 是唯一的 flush 信号。`study/audio_flow/` 为了学习简化故意没 flush swr，但在代码里写明了这是"学习简化"。

---

### Bug D: 跨线程共用 AVFormatContext 导致偶发崩溃

**Situation**
一个视频预览工具：main thread 负责 decoder + 显示，worker thread 跑 `av_read_frame` 喂一个 packet queue。压力测试 1 小时后偶发崩溃在 `demux.c` 内部。

**Task**
定位崩溃原因。

**Action**
崩溃堆栈显示 `av_read_frame` → `read_frame_internal` → 访问 `si->packet_buffer.head` 时指针值异常。检查代码发现：
- Worker thread 在循环 `av_read_frame`
- Main thread 偶尔调 `avformat_seek_file` 响应用户拖动进度条

两个线程同时操作 `AVFormatContext::packet_buffer`（seek 会清空，read 会追加），没有加锁。这是**数据竞争**。

修复：加一个 `std::mutex` 保护所有对 `AVFormatContext` 的访问。seek 和 read 必须串行。

**Result**
- 压力测试 24 小时无崩溃
- **核心教训**：**FFmpeg 所有的 `AVFormatContext` 和 `AVCodecContext` 操作都不是线程安全的**。不是只有 write 不安全，read + seek 并发也不安全。文档里写的 "threadsafety" 表只覆盖数据结构字段，不覆盖函数调用序列。

---

### Bug E: 旧 `AVCodecContext::channels` 和新 `ch_layout` 双写导致状态不一致

**Situation**
一个旧代码库从 FFmpeg 4.x 升到 6.x。编译器有 deprecation warning 说 `AVCodecContext::channels` 被弃用，但旧代码到处是：
```c
enc_ctx->channels = 2;
enc_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
```
简单改成：
```c
enc_ctx->channels = 2;
enc_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
av_channel_layout_default(&enc_ctx->ch_layout, 2);  // 新 API 并存
```
结果某些 encoder（特别是 opus）初始化后行为异常，输出声道错乱。

**Task**
找出新旧 API 并存时的问题。

**Action**
阅读 `libavcodec/version.h` 和 opus encoder 源码发现：
- 旧字段 `channels`/`channel_layout` 在 FFmpeg 6.x 仍然存在但标记 deprecated
- 新字段 `ch_layout` 是权威来源
- **FFmpeg 内部会尝试从旧字段同步到新字段**，但如果两者都被用户设置且不一致，**行为未定义**
- 具体说：opus encoder 先读 `ch_layout`，如果它有效就用；如果用户同时设置了旧字段，两者不一致时可能覆盖也可能不覆盖

修复：彻底删除所有旧 API 用法，只设 `ch_layout`：
```c
av_channel_layout_default(&enc_ctx->ch_layout, 2);
```
或者更保险：`enc_ctx->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;`

**Result**
- Opus 输出正常
- **核心教训**：FFmpeg API 升级时**永远不要新旧并存**。认真读 deprecation 说明，要么全用新的，要么全用旧的。旧字段 5.1 版本后属于"只读兼容",不再写入是安全的，但用户写它可能和内部同步逻辑冲突。

---

## 5. FFmpeg 源码中的 5 个真实 bug

以下 bug 均来自 FFmpeg 上游 git 历史（可在提交时的 `git show <sha>` 验证）。

### Source Bug #1: AAC USAC decoder — 标签映射缓存 use-after-free

**Commit：** `d6458f6a8b`
**File：** `libavcodec/aac/aacdec.c:164-169`

**缺陷：**
AAC USAC decoder 在流格式变化时会重新配置声道元素（`ChannelElement`）。`che_configure` 会释放旧的 `ac->che[type][id]`，但漏了一个**缓存数组** `ac->tag_che_map`：它按 tag 索引 `ChannelElement` 指针，可能存有跨类型引用（比如 TYPE_SCE 的 tag 映到 TYPE_LFE 的元素）。被释放的元素的地址还留在 cache 里。

**影响：**
后续 `ff_aac_get_che()` 返回悬挂指针，在 `decode_usac_core_coder()` 中 deref 时 UAF。OSS-Fuzz 报告了这个问题（#440220467），真实 USAC 流在格式切换时触发。

**修复：**
在 `che_configure` 释放元素前，扫描整个 `tag_che_map` 把所有指向它的 entry 置 NULL：
```c
for (int i = 0; i < FF_ARRAY_ELEMS(ac->tag_che_map); i++) {
    for (int j = 0; j < MAX_ELEM_ID; j++) {
        if (ac->tag_che_map[i][j] == ac->che[type][id])
            ac->tag_che_map[i][j] = NULL;
    }
}
```

**教训：**
释放对象时要**清除所有引用**而不仅是主引用。任何缓存结构都是潜在的悬挂指针源。这是经典的"多重所有权"bug——缓存以为自己只是"弱引用"，但没有机制让它知道目标被释放了。

---

### Source Bug #2: QDM2 decoder — 错误恢复路径状态未重置

**Commit：** `a795ca89fa`
**File：** `libavcodec/qdm2.c:1932-1933`

**缺陷：**
QDM2 是 Cook 族的有损音频 codec。decoder 用 `sub_packet` 索引跟踪多 packet 组合帧的进度。**错误时没重置**：如果解码某个 packet 出错，下次进来时 `sub_packet` 还是非零，跳过了 `qdm2_decode_super_block` 的初始化步骤——这个函数负责把 packet list 指针指向当前 packet 的数据。跳过它 = 继续使用上次 packet（已释放）的指针。

**影响：**
UAF。错误恢复路径上触发。攻击者构造一个先坏后好的 QDM2 流即可触发。

**修复：**
一行：
```c
s->sub_packet = 0;  // 在 qdm2_decode_frame 开头
```

**教训：**
**状态机在错误边界必须复位**。decoder 的状态不能假设"上次调用正常完成"。帧边界是天然的复位点，在那里主动清状态是防御性编程的基本功。这类 bug 特别阴险——正常路径测试完全覆盖不到。

---

### Source Bug #3: FLAC decoder — 33-bit LPC 解码有符号整数溢出

**Commit：** `fd7352660b`
**File：** `libavcodec/flacdec.c:516`

**缺陷：**
FLAC 33-bit LPC subframe 的累加核心：
```c
sum += (int64_t)coeffs[j] * decoded[j];
```
`coeffs` 和 `decoded` 都是 32-bit 整数，相乘结果用 `int64_t` 存。正常情况够用。但构造特定值让乘积超过 `int64_t` 范围就是**有符号整数溢出 = C 未定义行为**。

**影响：**
OSS-Fuzz #45982 发现。编译器激进优化下可能产生任意行为：计算错误、crash，在 `-fsanitize=signed-integer-overflow` 下立即触发。安全敏感场景（VoIP、浏览器 media 解码）下是真实漏洞。

**修复：**
把一个操作数改成 `uint64_t`：
```c
sum += (int64_t)coeffs[j] * (uint64_t)decoded[j];
```
C 里 `signed * unsigned` 结果是 unsigned，溢出行为定义为模 `2^64` 环绕。然后再赋值给 `int64_t` 触发 implementation-defined 转换（x86/ARM 都按位拷贝）。

**教训：**
**C 里有符号溢出是 UB，无符号溢出是 well-defined。** 信号处理代码（滤波累加、MDCT）是整数溢出的高发区。统一的修复模式：**累加用 unsigned，最后转回 signed**。这个 bug 在 FFmpeg FLAC decoder 里同一时期修了三四个类似的（`112a077d06`、`35e6960a6b`），说明是系统性问题。

---

### Source Bug #4: Speex decoder — frame_size 整数溢出导致堆溢出

**Commit：** `66b50445cb`
**File：** `libavcodec/speexdec.c:1423-1425`

**缺陷：**
Speex decoder 读 extradata 里的 frame_size：
```c
s->frame_size = (1 + (s->mode > 0)) * bytestream_get_le32(&buf);
```
`frame_size` 是 `int`。`bytestream_get_le32` 返回 32 位无符号值，当 `mode > 0` 时乘 2，结果**赋给 int 会截断**。构造一个大 frame_size 让乘 2 后的值溢出到很小的正数。

**影响：**
后续堆分配 `av_malloc(s->frame_size * ...)` 基于这个被截断的小值，分配的 buffer 实际容不下真实数据。解码时写入导致**堆缓冲区溢出**。ticket #10866 报告的。

**修复：**
**先验证，再乘法**：
```c
s->frame_size = bytestream_get_le32(&buf);
if (s->frame_size < NB_FRAME_SIZE << (s->mode > 0))
    return AVERROR_INVALIDDATA;
s->frame_size *= 1 + (s->mode > 0);
```

**教训：**
**永远不要在验证之前对不可信输入做算术运算。** 正确的 parser 模式是"读值 → 验证范围 → 使用"。这个 bug 违反的是第二步：先做了乘法再用，溢出已经发生。解析器（demuxer、codec extradata parser）是安全边界，每一个从 bitstream 读出的值都要当做敌对输入对待。

---

### Source Bug #5: libswresample — 线性重采样累加溢出

**Commit：** `17cad7ac75`
**File：** `libswresample/resample_template.c:28-30, 55-57, 69-71`

**缺陷：**
libswresample 的线性 resampler 核心循环：
```c
FELEM2 val = FOFFSET, v2 = FOFFSET;
for (i = 0; i < c->filter_length; i++) {
    val += src[sample_index + i] * (FELEM2)filter[i];
    v2  += src[sample_index + i] * (FELEM2)filter[i + c->filter_alloc];
}
```
`FELEM2` 是有符号类型（取决于 precision，通常 `int32_t` 或 `int64_t`）。极端输入 + 长滤波器时，累加 `val` 可能溢出——又是 UB。

**影响：**
构造的音频输入能让 swresample 产生错误输出或 crash。影响所有用 libswresample 的应用（包括我们的 `study/audio_flow`）。

**修复：**
引入新宏 `FELEM2U`（对应的无符号类型）用于累加：
```c
FELEM2U val = FOFFSET, v2 = FOFFSET;
// ...累加...
OUT(dst[dst_index], (FELEM2)val);  // 存储时转回有符号
```

**教训：**
和 #3 同一个模式：**信号处理热路径的累加用 unsigned 是业界标准做法**。FFmpeg 里 FLAC、swresample、dct 都修过类似问题。这也提示做音频算法时：
1. 不要用 `int` 类型累加，要明确选 `int32_t`/`int64_t`/`uint64_t`
2. 能估算上界的话用上界选类型
3. 跑 UBSan/CI 检测 UB

---

## 附录：面试加分提示

**如果面试官问得浅（"你了解 FFmpeg 吗"）：**
- 画这张图：`URL → avformat → AVPacket → avcodec → AVFrame → swresample → 输出`
- 强调分层解耦的好处（remux vs transcode）

**如果面试官问得深：**
- 聊 `send_packet/receive_frame` 的 push 模型取代旧 `decode_audio4` 的动机
- 聊 `AVChannelLayout` 5.1 重构的理由
- 聊 `av_frame_unref` vs `av_frame_free` 的语义
- 聊为什么 MP3 永远 1152 samples（MDCT 窗口长度决定的）

**如果面试官问安全 / 稳定性相关：**
- 引用本文 Source Bug #3/#4/#5 讲整数溢出
- 讲 Bug D 里的线程安全陷阱
- 讲 demuxer 是安全边界，每个字段都要验证

**如果面试官让你口述一个完整的音频播放器代码结构：**
- 看 `study/audio_flow/main.c`——这是一份可以 5 分钟内在白板上写出来的版本
- 6 个步骤 printf 是天然的讲解 scaffold

---

**文档结束。**
