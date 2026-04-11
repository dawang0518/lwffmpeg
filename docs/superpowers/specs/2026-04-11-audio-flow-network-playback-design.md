# audio_flow — 网络音频播放示例

**Status:** Approved
**Date:** 2026-04-11
**Location:** `study/audio_flow/`

## 1. 目的

为 `study/` 学习目录添加第二个可运行示例，在已有 `video_flow`（demux→decode→保存帧）的基础上，补齐 **音频支路** 和 **网络输入** 两个维度，让完整流程真正能"在电脑上听到"。

教学目标（按重要性）：

1. **`swresample`** 的完整用法 —— 与 `video_flow` 里 `swscale` 对称的 API
2. **网络协议层**（`avformat_open_input` 接 HTTP URL）
3. **音频播放后端** 与 ffmpeg 的衔接 —— 其他数据从哪来、送到哪去
4. 音频帧与视频帧在 API 上的差异（planar vs packed、`nb_samples` 等）

## 2. 范围

### In scope
- 一个可执行的 C 程序，从 HTTP URL（或本地文件）播放音频到系统扬声器
- CMake 构建，照搬 `video_flow` 的 pkg-config 模式
- 命令行运行，打印每一步的状态

### Out of scope
- 音量控制、暂停、跳转
- 图形界面 / 进度条
- 多线程 / 环形缓冲区（见 §4.1 设计决策）
- HLS / DASH / Icecast 持续流（见 §4.4）
- 音频可视化

## 3. 目录结构

```
study/audio_flow/
├── CMakeLists.txt
└── main.c
```

## 4. 设计决策

### 4.1 SDL 用 `SDL_QueueAudio` 而不是 callback

**决策：** 使用 push 模式（`SDL_QueueAudio`），不用 callback 模式。

**理由：**
- Callback 模式需要自己写**环形缓冲区 + 互斥锁**，代码量是主流程的 2 倍
- 学习目标是 ffmpeg，不是写音频环形缓冲区
- Push 模式是线性的：`decode → resample → SDL_QueueAudio → 下一帧`，无多线程，无共享状态

**代价：**
- 需要手动节流：队列超过 1MB 时 `SDL_Delay(10)` 等待，否则解码比播放快会导致内存膨胀

### 4.2 输出格式固定为 S16 interleaved / 44100Hz / stereo

**决策：** 不管输入音频是什么，都用 `swresample` 转到 `AV_SAMPLE_FMT_S16` / `44100` / `AV_CH_LAYOUT_STEREO`。

**理由：**
- MP3 解码出来通常是 `fltp`（planar float），SDL 需要 packed S16 —— 这正是 `swresample` 存在的理由
- 固定目标格式简化 SDL 初始化，学习焦点留给 ffmpeg
- 44100Hz 是 CD 标准，所有音频设备都支持

### 4.3 单线程 + 节流循环

主循环：
```
while (av_read_frame) {
    if (非音频流) continue;
    avcodec_send_packet;
    while (avcodec_receive_frame) {
        swr_convert;
        SDL_QueueAudio;
        while (queued > 1MB) SDL_Delay(10);
    }
}
```

结束后用 `while (SDL_GetQueuedAudioSize > 0) SDL_Delay(100)` 等队列放完。

### 4.4 默认 URL 硬编码，argv 可覆盖

```c
const char *url = argc > 1 ? argv[1] :
    "https://www.soundhelix.com/examples/mp3/SoundHelix-Song-1.mp3";
```

**为什么选 SoundHelix：** 公开 CC 授权、稳定十几年、8.5MB 约 6 分钟、content-length 明确（便于探测流信息）。

**本地文件：** `av_find_best_stream` / `avformat_open_input` 同样接受本地路径，不需要特殊处理。

### 4.5 错误处理模式

- 所有 ffmpeg 调用用 `av_err2str` 打印错误
- 单一 `cleanup:` 标签，按分配逆序释放：`swr_free` → `avcodec_free_context` → `avformat_close_input` → `SDL_CloseAudioDevice` → `SDL_Quit`
- 所有资源在声明时初始化为 NULL / -1，保证 goto cleanup 时释放安全

## 5. 数据流

```
HTTP URL (SoundHelix .mp3)
  │
  ▼ avformat_open_input / avformat_find_stream_info
AVFormatContext (mp3 demuxer + HTTP protocol)
  │
  ▼ av_find_best_stream(AUDIO)
AVStream → AVCodecContext (mp3 decoder)
  │
  ▼ av_read_frame → avcodec_send_packet → avcodec_receive_frame
AVFrame (PCM, 格式通常是 fltp planar float, 44100, stereo)
  │
  ▼ swr_convert  [核心学习点]
PCM S16 interleaved, 44100Hz, 2ch (bytes buffer)
  │
  ▼ SDL_QueueAudio(dev, buf, bytes)
SDL 内部队列
  │
  ▼ SDL audio thread (内部)
CoreAudio → 扬声器 🔊
```

## 6. 主要 API 使用

| 步骤 | API | 说明 |
|---|---|---|
| 网络初始化 | `avformat_network_init` | 启用 http/https 协议 |
| 打开输入 | `avformat_open_input` | URL 作为路径传入 |
| 探测流 | `avformat_find_stream_info` | 需要先读一些数据 |
| 找音频流 | `av_find_best_stream(AVMEDIA_TYPE_AUDIO)` | 自动挑最合适的 |
| 解码器 | `avcodec_find_decoder` + `avcodec_alloc_context3` + `avcodec_parameters_to_context` + `avcodec_open2` | 标准四步 |
| 读取 | `av_read_frame` + `avcodec_send_packet` + `avcodec_receive_frame` | send/receive 新 API |
| 重采样 | `swr_alloc_set_opts2` + `swr_init` + `swr_convert` + `swr_get_delay` | 核心点 |
| 输出分配 | `av_samples_alloc` + `av_rescale_rnd` | 计算输出采样数 |
| SDL | `SDL_Init(AUDIO)` + `SDL_OpenAudioDevice` + `SDL_PauseAudioDevice(dev,0)` + `SDL_QueueAudio` + `SDL_GetQueuedAudioSize` | 5 个调用即可 |

## 7. CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)
project(audio_flow C)

set(CMAKE_C_STANDARD 11)

set(ENV{PKG_CONFIG_PATH} "${CMAKE_SOURCE_DIR}/../../ffmpeg-build/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswresample)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

add_executable(audio_flow main.c)
target_link_libraries(audio_flow PRIVATE PkgConfig::FFMPEG PkgConfig::SDL2)
```

注意：`libswresample` 替换了 `video_flow` 里的 `libswscale`；新增 `sdl2`。

## 8. 预期输出

```
=== 第1步: 打开网络输入 ===
  URL: https://www.soundhelix.com/examples/mp3/SoundHelix-Song-1.mp3

=== 第2步: 探测流 ===
  格式: mp3
  时长: 373.04 秒
  流数量: 1
  流 #0: 类型=audio, 编码=mp3

=== 第3步: 打开解码器 ===
  解码器: mp3 (MP3 (MPEG audio layer 3))
  采样率: 44100 Hz
  声道数: 2
  样本格式: fltp

=== 第4步: 配置 SwrContext ===
  输入:  44100 Hz | fltp        | stereo
  输出:  44100 Hz | s16         | stereo

=== 第5步: 初始化 SDL 音频设备 ===
  设备已打开: 44100Hz S16 2ch

=== 第6步: 解码 + 播放 (Ctrl+C 中断) ===
  [frame 0] 1152 samples → 1152 out, queued 4.5KB
  [frame 10] 1152 samples → 1152 out, queued 840KB (节流中)
  ...

=== 完成 ===
```

## 9. 测试 / 验收标准

1. **编译通过**：`cmake -B build && cmake --build build` 无错误无 warning
2. **能播放**：运行后扬声器听到 SoundHelix 的示例曲，无卡顿（除非网络差）
3. **打印完整**：前 5 步的状态信息都打印
4. **干净退出**：播完后正常返回 0，无内存泄漏崩溃
5. **Ctrl+C 可中断**：中断后也能干净退出（SDL 会收到信号）

## 10. 依赖前置条件

- [x] `ffmpeg-build/` 存在且已编译（上一轮已完成）
- [x] FFmpeg 启用了 `libavformat`/`libavcodec`/`libavutil`/`libswresample` 和 HTTP/HTTPS 协议（已用 `ffmpeg -protocols` 验证：http, https 均在支持列表中）
- [x] 系统安装了 SDL2 2.32.10（已在 `/opt/homebrew` 确认）
- [x] 系统安装了 `pkg-config`（`video_flow` 已在用）

## 11. 已考虑但拒绝的方案

| 方案 | 拒绝原因 |
|---|---|
| **AudioQueue / AudioToolbox** 直接输出 | 用户选了 SDL2 |
| **libavdevice audiotoolbox 输出** | 代码 10 行，几乎没有学习价值 |
| **SDL callback + ring buffer** | 对学 ffmpeg 本身是干扰 |
| **多线程解码 + 播放** | YAGNI，单线程 + 节流已经够流畅 |
| **进度条 / TUI** | 超出 "学 ffmpeg API" 的范围 |
| **暂停/跳转/音量控制** | 同上，属于播放器功能 |
