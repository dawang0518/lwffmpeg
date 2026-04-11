# audio_flow — 网络音频播放 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建 `study/audio_flow/`，一个用 FFmpeg 解复用/解码网络 MP3 并用 SDL2 播放到系统扬声器的 C 程序；作为 `video_flow` 之后的第二个学习示例，重点演练 swresample 和网络输入。

**Architecture:** 单线程线性流水线：`avformat_open_input(HTTP URL) → av_read_frame → avcodec_send_packet/receive_frame → swr_convert → SDL_QueueAudio`。SDL 用 push 模式（队列超 1MB 时 `SDL_Delay` 节流）以避免环形缓冲区和锁。代码分两个文件：`CMakeLists.txt`（照搬 `video_flow` 模式 + 新增 `sdl2`）和 `main.c`（单文件，按编号步骤打印状态）。

**Tech Stack:** C11, FFmpeg 7.x (libavformat/libavcodec/libavutil/libswresample), SDL2 2.32, CMake 3.20, pkg-config

**TDD 适配说明：** 这是一个与外部系统（网络、音频设备）紧密耦合的学习型 C 程序。纯单元 TDD 在该场景下不切实际，且本 repo 无测试框架。本计划采用 **增量编译-运行验证** 替代 TDD 循环：每个 task 完成后都必须产出能 `cmake --build` 且能运行的中间产物，并用一条具体命令 + 预期输出作为验收。参考 `study/video_flow/` 的构建模式。

**Spec:** `docs/superpowers/specs/2026-04-11-audio-flow-network-playback-design.md`

---

## 文件结构

| 路径 | 职责 | 创建/修改 |
|---|---|---|
| `study/audio_flow/CMakeLists.txt` | CMake 构建脚本，通过 pkg-config 链接本地 FFmpeg + SDL2 | 创建 |
| `study/audio_flow/main.c` | 完整程序入口，打印 6 步状态并播放音频 | 创建 |

无头文件、无 lib 拆分 —— 与 `video_flow` 保持一致。

---

## Task 1: 项目脚手架与 Hello World 构建

**目标：** 建立能编译、能链接 FFmpeg+SDL2、能运行的最小可执行文件。确认 pkg-config 能同时找到两组库。

**Files:**
- Create: `study/audio_flow/CMakeLists.txt`
- Create: `study/audio_flow/main.c`

- [ ] **Step 1: 创建目录**

Run:
```bash
mkdir -p /Users/admin/workspace/me/lwffmpeg/study/audio_flow
```

Expected: 无输出（目录创建成功）。

- [ ] **Step 2: 写 CMakeLists.txt**

Create `study/audio_flow/CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.20)
project(audio_flow C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # 让 clangd 找到 include 路径

# 复用本地编译的 FFmpeg (同 video_flow)
set(ENV{PKG_CONFIG_PATH} "${CMAKE_SOURCE_DIR}/../../ffmpeg-build/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswresample
)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

add_executable(audio_flow main.c)
target_link_libraries(audio_flow PRIVATE PkgConfig::FFMPEG PkgConfig::SDL2)
```

- [ ] **Step 3: 写最小 main.c（只做 include + printf）**

Create `study/audio_flow/main.c`：

```c
/**
 * audio_flow - 网络音频播放学习示例
 *
 * 流程: avformat_open_input(HTTP) -> decode -> swresample -> SDL_QueueAudio
 *
 * 用法: ./audio_flow [url-or-file]
 *       默认播放 SoundHelix 的公开测试 MP3
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <SDL.h>

#define DEFAULT_URL "https://www.soundhelix.com/examples/mp3/SoundHelix-Song-1.mp3"

int main(int argc, char **argv) {
    const char *url = (argc > 1) ? argv[1] : DEFAULT_URL;
    printf("audio_flow scaffolding OK\n");
    printf("  URL: %s\n", url);
    printf("  FFmpeg: %s\n", av_version_info());
    printf("  SDL2:   %d.%d.%d\n",
           SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
    return 0;
}
```

- [ ] **Step 4: 首次构建**

Run:
```bash
cd /Users/admin/workspace/me/lwffmpeg/study/audio_flow && \
cmake -B build && cmake --build build 2>&1
```

Expected:
- 最后一行是 `[100%] Built target audio_flow`
- 无 warning、无 error
- 若 pkg-config 找不到 sdl2，提示先 `brew install sdl2`（spec §10 已确认系统已装 2.32.10）

- [ ] **Step 5: 运行脚手架**

Run:
```bash
cd /Users/admin/workspace/me/lwffmpeg/study/audio_flow && ./build/audio_flow
```

Expected（版本号视环境而定）：
```
audio_flow scaffolding OK
  URL: https://www.soundhelix.com/examples/mp3/SoundHelix-Song-1.mp3
  FFmpeg: 7.x.x
  SDL2:   2.32.10
```

- [ ] **Step 6: Symlink compile_commands.json（让 clangd 看到 include）**

Run:
```bash
cd /Users/admin/workspace/me/lwffmpeg/study/audio_flow && \
ln -sf build/compile_commands.json compile_commands.json
```

Expected: 在源目录下生成一个指向 `build/compile_commands.json` 的 symlink。clangd 会沿着源文件父目录向上查找这个文件。

- [ ] **Step 7: Commit**

```bash
cd /Users/admin/workspace/me/lwffmpeg && \
git add study/audio_flow/CMakeLists.txt study/audio_flow/main.c study/audio_flow/compile_commands.json && \
git commit -m "feat(audio_flow): scaffold CMake + main.c linking FFmpeg and SDL2"
```

---

## Task 2: 打开网络输入并探测流信息

**目标：** 让程序真正访问 HTTP URL，打印容器格式、时长、流数量。不处理音频数据，验证 ffmpeg 网络协议工作正常。

**Files:**
- Modify: `study/audio_flow/main.c`

- [ ] **Step 1: 在 main.c 顶部添加错误宏和 cleanup 骨架**

替换整个 `main.c` 为下列内容（保留 includes + DEFAULT_URL，主体重写）：

```c
/**
 * audio_flow - 网络音频播放学习示例
 *
 * 流程: avformat_open_input(HTTP) -> decode -> swresample -> SDL_QueueAudio
 *
 * 用法: ./audio_flow [url-or-file]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <SDL.h>

#define DEFAULT_URL "https://www.soundhelix.com/examples/mp3/SoundHelix-Song-1.mp3"

#define CHECK_AV(expr, msg)                                                    \
    do {                                                                       \
        int _ret = (expr);                                                     \
        if (_ret < 0) {                                                        \
            char _errbuf[AV_ERROR_MAX_STRING_SIZE];                            \
            av_strerror(_ret, _errbuf, sizeof(_errbuf));                       \
            fprintf(stderr, "[ERR] %s: %s\n", (msg), _errbuf);                 \
            goto cleanup;                                                      \
        }                                                                      \
    } while (0)

int main(int argc, char **argv) {
    const char *url = (argc > 1) ? argv[1] : DEFAULT_URL;
    int exit_code = 1;

    AVFormatContext *fmt_ctx = NULL;

    /* === 第1步: 打开网络输入 === */
    /* 注意: 下面的 avformat_network_init 必须在任何 goto cleanup 之前,
     * 因为 cleanup 里的 avformat_network_deinit 是无条件调用的. */
    printf("=== 第1步: 打开网络输入 ===\n");
    printf("  URL: %s\n", url);
    avformat_network_init();
    CHECK_AV(avformat_open_input(&fmt_ctx, url, NULL, NULL),
             "avformat_open_input");

    /* === 第2步: 探测流信息 === */
    printf("\n=== 第2步: 探测流信息 ===\n");
    CHECK_AV(avformat_find_stream_info(fmt_ctx, NULL),
             "avformat_find_stream_info");
    printf("  容器格式: %s\n", fmt_ctx->iformat->name);
    printf("  时长: %.2f 秒\n", (double)fmt_ctx->duration / AV_TIME_BASE);
    printf("  流数量: %u\n", fmt_ctx->nb_streams);
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream *st = fmt_ctx->streams[i];
        const char *type = av_get_media_type_string(st->codecpar->codec_type);
        const char *codec = avcodec_get_name(st->codecpar->codec_id);
        printf("  流 #%u: 类型=%s, 编码=%s\n", i,
               type ? type : "unknown", codec);
    }

    exit_code = 0;

cleanup:
    avformat_close_input(&fmt_ctx);
    avformat_network_deinit();
    return exit_code;
}
```

(FFmpeg 的 `*_close_input` / `*_free_context` / `swr_free` 都安全接受 NULL，因此无需 `if` 守卫。)

- [ ] **Step 2: 重新构建**

Run:
```bash
cd /Users/admin/workspace/me/lwffmpeg/study/audio_flow && cmake --build build 2>&1
```

Expected: `[100%] Built target audio_flow`，无 warning（`-Wunused` 等都应清理）。

- [ ] **Step 3: 运行验证网络输入**

Run:
```bash
cd /Users/admin/workspace/me/lwffmpeg/study/audio_flow && ./build/audio_flow
```

Expected（时长精确值可能在 373 秒附近）：
```
=== 第1步: 打开网络输入 ===
  URL: https://www.soundhelix.com/examples/mp3/SoundHelix-Song-1.mp3

=== 第2步: 探测流信息 ===
  容器格式: mp3
  时长: 373.04 秒
  流数量: 1
  流 #0: 类型=audio, 编码=mp3
```

失败诊断：
- `avformat_open_input: Protocol not found` → ffmpeg 没编入 http 协议（已验证不会）
- `avformat_open_input: Server returned 4XX` → URL 失效，换一个
- 超时 → 换一个 URL 或本地 mp3

- [ ] **Step 4: Commit**

```bash
cd /Users/admin/workspace/me/lwffmpeg && \
git add study/audio_flow/main.c && \
git commit -m "feat(audio_flow): open network input and probe stream info"
```

---

## Task 3: 查找音频流并打开解码器

**目标：** 用 `av_find_best_stream` 挑出音频流，按标准四步（find/alloc/copy/open）打开解码器，打印解码器信息。不解码数据。

**Files:**
- Modify: `study/audio_flow/main.c`

- [ ] **Step 1: 扩展 main.c 添加第 3 步逻辑**

在 Task 2 的 main.c 基础上：

（a）在 `AVFormatContext *fmt_ctx = NULL;` 下方添加资源声明：
```c
    AVCodecContext *dec_ctx = NULL;
    int audio_stream_idx = -1;
```

（b）找到 `exit_code = 0;` 这一行，**在它上方**（紧挨着它，`}` 之后 / `exit_code = 0` 之前）插入第3步代码块。`exit_code = 0;` 保持在 cleanup 标签前作为最后一行：

```c
    /* === 第3步: 查找音频流并打开解码器 === */
    printf("\n=== 第3步: 查找音频流并打开解码器 ===\n");
    audio_stream_idx = av_find_best_stream(
        fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (audio_stream_idx < 0) {
        fprintf(stderr, "[ERR] 未找到音频流\n");
        goto cleanup;
    }
    printf("  音频流索引: %d\n", audio_stream_idx);

    AVStream *audio_stream = fmt_ctx->streams[audio_stream_idx];
    const AVCodec *decoder = avcodec_find_decoder(audio_stream->codecpar->codec_id);
    if (!decoder) {
        fprintf(stderr, "[ERR] 找不到解码器 (codec_id=%d)\n",
                audio_stream->codecpar->codec_id);
        goto cleanup;
    }

    dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) {
        fprintf(stderr, "[ERR] avcodec_alloc_context3 失败\n");
        goto cleanup;
    }
    CHECK_AV(avcodec_parameters_to_context(dec_ctx, audio_stream->codecpar),
             "avcodec_parameters_to_context");
    CHECK_AV(avcodec_open2(dec_ctx, decoder, NULL), "avcodec_open2");

    /* long_name 在 FFmpeg --enable-small 编译时为 NULL; name 总是有效 */
    printf("  解码器: %s (%s)\n", decoder->name,
           decoder->long_name ? decoder->long_name : "");
    printf("  采样率: %d Hz\n", dec_ctx->sample_rate);
    printf("  声道数: %d\n", dec_ctx->ch_layout.nb_channels);
    const char *sfmt_name = av_get_sample_fmt_name(dec_ctx->sample_fmt);
    printf("  样本格式: %s\n", sfmt_name ? sfmt_name : "unknown");
```

（c）在 `cleanup:` 标签下，在 `avformat_close_input` **之前** 插入：
```c
    avcodec_free_context(&dec_ctx);
```

- [ ] **Step 2: 构建**

Run:
```bash
cd /Users/admin/workspace/me/lwffmpeg/study/audio_flow && cmake --build build 2>&1
```

Expected: `[100%] Built target audio_flow`。

注意：FFmpeg 7.x 用 `AVChannelLayout ch_layout` 替代老 API 的 `channels` 字段。如果构建报 `'AVCodecContext' has no member named 'ch_layout'`，说明 FFmpeg 版本 < 5.1，需要改成 `dec_ctx->channels`。先用 `av_version_info()` 的输出确认版本。

- [ ] **Step 3: 运行验证**

Run:
```bash
cd /Users/admin/workspace/me/lwffmpeg/study/audio_flow && ./build/audio_flow
```

Expected（接在 Task 2 的输出之后）：
```
=== 第3步: 查找音频流并打开解码器 ===
  音频流索引: 0
  解码器: mp3 (MP3 (MPEG audio layer 3))
  采样率: 44100 Hz
  声道数: 2
  样本格式: fltp
```

- [ ] **Step 4: Commit**

```bash
cd /Users/admin/workspace/me/lwffmpeg && \
git add study/audio_flow/main.c && \
git commit -m "feat(audio_flow): locate audio stream and open decoder"
```

---

## Task 4: 配置 SwrContext

**目标：** 初始化一个从解码器原生格式（通常 `fltp/44100/stereo`）到 SDL 目标格式（`s16/44100/stereo`）的 SwrContext。不做数据转换，只确认 `swr_init` 成功。

**Files:**
- Modify: `study/audio_flow/main.c`

- [ ] **Step 1: 在全局区下方添加目标格式常量**

在 `#define DEFAULT_URL ...` 下方添加：

```c
/* SDL 输出目标格式（固定） */
#define OUT_SAMPLE_RATE 44100
#define OUT_SAMPLE_FMT  AV_SAMPLE_FMT_S16
#define OUT_CHANNELS    2
```

- [ ] **Step 2: 添加 SwrContext 资源声明**

在 `int audio_stream_idx = -1;` 下方添加：
```c
    SwrContext *swr_ctx = NULL;
```

- [ ] **Step 3: 在第3步结尾（解码器打印之后、`exit_code = 0;` 之前）插入第4步**

```c
    /* === 第4步: 配置 SwrContext === */
    printf("\n=== 第4步: 配置 SwrContext ===\n");

    AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO;

    CHECK_AV(swr_alloc_set_opts2(
                 &swr_ctx,
                 &out_ch_layout, OUT_SAMPLE_FMT, OUT_SAMPLE_RATE,
                 &dec_ctx->ch_layout, dec_ctx->sample_fmt, dec_ctx->sample_rate,
                 0, NULL),
             "swr_alloc_set_opts2");
    CHECK_AV(swr_init(swr_ctx), "swr_init");

    const char *in_fmt = av_get_sample_fmt_name(dec_ctx->sample_fmt);
    const char *out_fmt = av_get_sample_fmt_name(OUT_SAMPLE_FMT);
    printf("  输入:  %d Hz | %-6s | %d ch\n",
           dec_ctx->sample_rate,
           in_fmt ? in_fmt : "unknown",
           dec_ctx->ch_layout.nb_channels);
    printf("  输出:  %d Hz | %-6s | %d ch\n",
           OUT_SAMPLE_RATE,
           out_fmt ? out_fmt : "unknown",
           OUT_CHANNELS);
```

**Note:** 使用 `AV_CHANNEL_LAYOUT_STEREO` 宏初始化可省去 `av_channel_layout_default` + `av_channel_layout_uninit` 的成对调用（该宏展开为静态字面量，无需释放）。

- [ ] **Step 4: 在 cleanup 标签下添加 swr 释放**

在 `avcodec_free_context(&dec_ctx);` **之前** 插入：

```c
    swr_free(&swr_ctx);
```

(`swr_free` 与其他 FFmpeg free 函数一样安全接受 NULL 指针，无需 `if` 守卫。)

- [ ] **Step 5: 构建**

Run:
```bash
cd /Users/admin/workspace/me/lwffmpeg/study/audio_flow && cmake --build build 2>&1
```

Expected: 无 error。

- [ ] **Step 6: 运行**

Run:
```bash
cd /Users/admin/workspace/me/lwffmpeg/study/audio_flow && ./build/audio_flow
```

Expected（新增）：
```
=== 第4步: 配置 SwrContext ===
  输入:  44100 Hz | fltp   | 2 ch
  输出:  44100 Hz | s16    | 2 ch
```

- [ ] **Step 7: Commit**

```bash
cd /Users/admin/workspace/me/lwffmpeg && \
git add study/audio_flow/main.c && \
git commit -m "feat(audio_flow): set up swresample to target s16/44100/stereo"
```

---

## Task 5: 初始化 SDL 音频设备

**目标：** 打开一个 44100Hz S16 立体声的 SDL 音频设备，开始播放（此时队列为空所以是静音）。验证 SDL 能找到系统音频输出。

**Files:**
- Modify: `study/audio_flow/main.c`

- [ ] **Step 1: 在资源声明区添加 SDL 相关**

在 `SwrContext *swr_ctx = NULL;` 下方添加：
```c
    SDL_AudioDeviceID audio_dev = 0;
    int sdl_inited = 0;
```

- [ ] **Step 2: 在第4步结尾之后、`exit_code = 0;` 之前插入第5步**

```c
    /* === 第5步: 初始化 SDL 音频设备 === */
    printf("\n=== 第5步: 初始化 SDL 音频设备 ===\n");
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "[ERR] SDL_Init: %s\n", SDL_GetError());
        goto cleanup;
    }
    sdl_inited = 1;

    SDL_AudioSpec want = {0};
    want.freq = OUT_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = OUT_CHANNELS;
    want.samples = 1024;
    want.callback = NULL;  /* 使用 SDL_QueueAudio push 模式 */

    SDL_AudioSpec have;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev == 0) {
        fprintf(stderr, "[ERR] SDL_OpenAudioDevice: %s\n", SDL_GetError());
        goto cleanup;
    }
    printf("  设备: freq=%d format=0x%04x channels=%d samples=%d\n",
           have.freq, have.format, have.channels, have.samples);

    SDL_PauseAudioDevice(audio_dev, 0);  /* 0 = unpause, 开始消费队列 */
```

- [ ] **Step 3: 在 cleanup 标签中添加 SDL 释放**

在 cleanup 标签下，**最前面**（`if (swr_ctx)` 之前）添加：
```c
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    if (sdl_inited) SDL_Quit();
```

- [ ] **Step 4: 构建**

Run:
```bash
cd /Users/admin/workspace/me/lwffmpeg/study/audio_flow && cmake --build build 2>&1
```

Expected: 无 error。

- [ ] **Step 5: 运行**

Run:
```bash
cd /Users/admin/workspace/me/lwffmpeg/study/audio_flow && ./build/audio_flow
```

Expected（新增；程序仍然秒退不播放声音，因为没送数据）：
```
=== 第5步: 初始化 SDL 音频设备 ===
  设备: freq=44100 format=0x8010 channels=2 samples=1024
```

`format=0x8010` 是 `AUDIO_S16LSB`（macOS 小端 = S16SYS）。

- [ ] **Step 6: Commit**

```bash
cd /Users/admin/workspace/me/lwffmpeg && \
git add study/audio_flow/main.c && \
git commit -m "feat(audio_flow): open SDL audio device with S16/44100/stereo"
```

---

## Task 6: 解码循环 + SwrConvert + SDL_QueueAudio（核心任务，能听到声音）

**目标：** 实现完整的 packet → frame → 重采样 → 送 SDL 队列的循环。这是程序的核心，完成后运行就能听到音乐。加入内存节流防止解码过快。

**Files:**
- Modify: `study/audio_flow/main.c`

- [ ] **Step 1: 在资源声明区添加 packet/frame 指针**

在 `int sdl_inited = 0;` 下方添加：
```c
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
```

- [ ] **Step 2: 在第5步结尾之后、`exit_code = 0;` 之前插入第6步（分配 + 解码循环）**

```c
    /* === 第6步: 解码 + 播放 === */
    printf("\n=== 第6步: 解码 + 播放 (Ctrl+C 中断) ===\n");

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) {
        fprintf(stderr, "[ERR] 分配 packet/frame 失败\n");
        goto cleanup;
    }

    const int out_bytes_per_sample = OUT_CHANNELS * av_get_bytes_per_sample(OUT_SAMPLE_FMT);
    int64_t total_frames = 0;

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != audio_stream_idx) {
            av_packet_unref(pkt);
            continue;
        }

        int ret = avcodec_send_packet(dec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) {
            fprintf(stderr, "[WARN] avcodec_send_packet: %d\n", ret);
            continue;
        }

        while ((ret = avcodec_receive_frame(dec_ctx, frame)) >= 0) {
            /* 计算输出采样数 (考虑 swr 内部延迟) */
            int64_t delay = swr_get_delay(swr_ctx, dec_ctx->sample_rate);
            int out_samples = (int)av_rescale_rnd(
                delay + frame->nb_samples,
                OUT_SAMPLE_RATE, dec_ctx->sample_rate, AV_ROUND_UP);

            uint8_t *out_buf = NULL;
            int out_linesize = 0;
            int alloc_ret = av_samples_alloc(
                &out_buf, &out_linesize,
                OUT_CHANNELS, out_samples, OUT_SAMPLE_FMT, 0);
            if (alloc_ret < 0) {
                fprintf(stderr, "[ERR] av_samples_alloc\n");
                av_frame_unref(frame);
                goto cleanup;
            }

            int converted = swr_convert(
                swr_ctx, &out_buf, out_samples,
                (const uint8_t **)frame->extended_data, frame->nb_samples);
            if (converted < 0) {
                fprintf(stderr, "[ERR] swr_convert\n");
                av_freep(&out_buf);
                av_frame_unref(frame);
                goto cleanup;
            }

            int queue_bytes = converted * out_bytes_per_sample;
            if (SDL_QueueAudio(audio_dev, out_buf, queue_bytes) < 0) {
                fprintf(stderr, "[ERR] SDL_QueueAudio: %s\n", SDL_GetError());
                av_freep(&out_buf);
                av_frame_unref(frame);
                goto cleanup;
            }
            av_freep(&out_buf);

            /* 节流：队列 > 1MB 时等待，防止内存膨胀 */
            while (SDL_GetQueuedAudioSize(audio_dev) > 1024 * 1024) {
                SDL_Delay(10);
            }

            if ((total_frames % 50) == 0) {
                printf("  [frame %lld] in=%d out=%d queued=%uKB\n",
                       (long long)total_frames, frame->nb_samples, converted,
                       SDL_GetQueuedAudioSize(audio_dev) / 1024);
            }
            total_frames++;
            av_frame_unref(frame);
        }

        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF && ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "[ERR] avcodec_receive_frame: %s\n", errbuf);
            goto cleanup;
        }
    }

    /* 刷新解码器剩余帧 */
    avcodec_send_packet(dec_ctx, NULL);
    while (avcodec_receive_frame(dec_ctx, frame) >= 0) {
        av_frame_unref(frame);
    }

    /* 等 SDL 把队列里的音频全部播完 */
    printf("  等待播放完成...\n");
    while (SDL_GetQueuedAudioSize(audio_dev) > 0) {
        SDL_Delay(100);
    }

    printf("\n=== 完成: 共解码 %lld 帧 ===\n", (long long)total_frames);
```

此处不再写 `exit_code = 0;` —— 它已经在第 2 步的末尾作为"成功路径最后一行"存在，Task 2~5 每次都保留它。Task 6 把主循环插到它上方之后，它自然就成了整个 main 的最后一行（紧挨 `cleanup:`）。如果此时它不在那里，说明前面某个 Task 插错了位置，回去修正。

- [ ] **Step 3: 在 cleanup 标签中添加 packet/frame 释放**

在 `if (audio_dev)` **之前** 添加：
```c
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
```

- [ ] **Step 4: 构建**

Run:
```bash
cd /Users/admin/workspace/me/lwffmpeg/study/audio_flow && cmake --build build 2>&1
```

Expected: `[100%] Built target audio_flow`，无 warning。

- [ ] **Step 5: 运行并听音频（核心验收）**

Run:
```bash
cd /Users/admin/workspace/me/lwffmpeg/study/audio_flow && ./build/audio_flow
```

Expected：
- 扬声器能听到 SoundHelix 测试曲（约 6 分钟）
- 控制台大约每 50 帧打印一行解码进度
- 最后打印 `=== 完成: 共解码 ~14000 帧 ===`（具体数视 frame_size 而定）
- 退出码 0

**加速验收（短跑）：** 全曲 6 分钟太久，可以本地下载短片段测试：
```bash
cd /tmp && \
/Users/admin/workspace/me/lwffmpeg/ffmpeg-build/bin/ffmpeg -y -f lavfi \
  -i "sine=frequency=440:duration=3" -c:a libmp3lame test.mp3 2>&1 | tail -3 && \
/Users/admin/workspace/me/lwffmpeg/study/audio_flow/build/audio_flow /tmp/test.mp3
```
应该听到 3 秒 440Hz 正弦波。

- [ ] **Step 6: Commit**

```bash
cd /Users/admin/workspace/me/lwffmpeg && \
git add study/audio_flow/main.c && \
git commit -m "feat(audio_flow): decode, resample and queue audio to SDL"
```

---

## Task 7: 打印收尾 & 文档

**目标：** 在 `study/audio_flow/` 里补一份极简 README，指向 spec 和用法。不改代码。

**Files:**
- Create: `study/audio_flow/README.md`

- [ ] **Step 1: 写 README.md**

Create `study/audio_flow/README.md`：

```markdown
# audio_flow

网络 MP3 播放示例（FFmpeg + SDL2），`video_flow` 的音频版。

## 构建

```bash
cmake -B build && cmake --build build
```

依赖：本地编译的 FFmpeg 在 `../../ffmpeg-build/`（见项目根 `study/学习路线.md`），系统装了 SDL2（`brew install sdl2`）。

## 运行

```bash
./build/audio_flow                          # 默认 SoundHelix 测试 MP3
./build/audio_flow path/to/local.mp3        # 本地文件
./build/audio_flow https://example.com/x.mp3 # 任意 URL
```

## 学了什么

| 主题 | 对应 API |
|---|---|
| 网络协议 | `avformat_network_init`, `avformat_open_input` 直接接 URL |
| 音频解码 | `avcodec_send_packet` / `avcodec_receive_frame` 的 audio 路径 |
| 重采样 | `swr_alloc_set_opts2` / `swr_init` / `swr_convert` / `swr_get_delay` |
| 样本内存 | `av_samples_alloc` + `av_rescale_rnd` 计算输出采样数 |
| SDL 音频输出 | `SDL_OpenAudioDevice` + `SDL_QueueAudio` (push 模式，无回调无锁) |

详见 `docs/superpowers/specs/2026-04-11-audio-flow-network-playback-design.md`。
```

- [ ] **Step 2: Commit**

```bash
cd /Users/admin/workspace/me/lwffmpeg && \
git add study/audio_flow/README.md && \
git commit -m "docs(audio_flow): add README with build and learning summary"
```

---

## 完成验收清单

全部 Task 完成后，手工过一遍：

- [ ] `study/audio_flow/build/audio_flow` 存在且可执行
- [ ] `./build/audio_flow` 无参数运行，网络通畅的情况下能听到 SoundHelix 示例曲
- [ ] `./build/audio_flow /tmp/test.mp3`（440Hz 3 秒正弦）能听到正弦音
- [ ] 程序正常结束、退出码 0
- [ ] 中途 Ctrl+C 能正常退出（SDL 信号处理默认行为）
- [ ] `cmake --build build 2>&1 | grep -i warning` 无输出
- [ ] git log 有 7 个原子 commit（`git log --oneline study/audio_flow docs/superpowers/plans`）

## 回滚策略

如果 Task 6 的解码循环出现难以调试的崩溃：

1. `git log --oneline study/audio_flow/main.c` 找到 Task 5 的 commit
2. `git checkout <task5-sha> -- study/audio_flow/main.c` 回到 Task 5
3. 程序仍可编译运行（只是没有声音输出），作为逐步排查的基线
