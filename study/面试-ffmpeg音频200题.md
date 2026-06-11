# FFmpeg 音频 250 题（面试深度版）

> 本文档是 `面试-ffmpeg音频深度总结.md` 的配套题库。主文是体系化讲解，本文是"做完 250 题能深入理解 FFmpeg 音频栈"的刷题手册。
>
> 源码基准：`ffmpeg` submodule pinned to `554dcc2885`。下文所有文件路径均以 `libavformat/` / `libavcodec/` / `libswresample/` / `libavutil/` 为前缀，可直接在源码里 grep。

## 目录

| 章节 | 主题 | 题数 |
|---|---|---|
| **第一部分 · 基础 50 题** | 架构 / Demuxer / Decoder / 重采样 / SDL / 性能 / Debug | 50 |
| **3A** | 架构与 API 深化（AVClass/AVOption/BufferPool/ABI 演进） | 10 |
| **3B** | libavformat 深度（probe/parser/seek/ID3/Xing/LAME） | 25 |
| **3C** | libavcodec 通用机制（send-receive/AVCodecInternal/flush） | 25 |
| **3D** | MP3 内部机制（header/granule/bit reservoir/Huffman/IMDCT） | 20 |
| **3E** | AAC（LC/HE/USAC，ADTS/LATM，ASC，SBR/PS） | 20 |
| **3F** | Opus / Vorbis / FLAC | 20 |
| **3G** | libswresample（重采样/channel remap/dither/SOXR） | 20 |
| **3H** | 样本格式与时间基 | 10 |
| **3I** | 音频 I/O（ALSA/CoreAudio/PulseAudio/WASAPI） | 10 |
| **3J** | 应用层音频 bug（20 个"业务底线"场景） | 20 |
| **3K** | FFmpeg 源码历史上的音频 bug（20 个真实 commit） | 20 |
| **合计** | — | **250** |

> 每题都标注了相关源码文件和行号，line 号以 submodule pin `554dcc2885` 的状态为准。看完答案后去 grep 对应源码是吸收的关键。

---

# 第一部分：基础 50 题

> 这一部分是从主文 `面试-ffmpeg音频深度总结.md` 的 Section 3 同步过来的，作为后面 200 题的铺垫。先做完这 50 题建立基本感觉，再去啃 3A–3K 的深度题。

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


---

# 第二部分：200 题深度扩展

### 3A. 架构与 API 深化

**Q1. `-user_agent` / `-reconnect` 这些 CLI flag 是怎么透传到底层 HTTP protocol 的？**
靠 `AVOption` + `AVClass` 的链式查找。每个可选上下文（`AVFormatContext` / `URLContext` / HTTPContext）的结构体第一个字段必须是 `const AVClass *av_class`，`AVClass::option` 指向该层的 option 表，`AVClass::child_class_iterate` 告诉 framework "我还有哪些子类"。`av_opt_set("user_agent", ...)` 会递归 `opt_find` 进入子类，`libavformat/http.c` 的 `HTTPContext` 里定义的 `{"user_agent", ..., offsetof(HTTPContext, user_agent)}` 就能被匹配上。这套机制让 ffmpeg.c 的 CLI 解析完全不认识 HTTP——只把 key/value 塞进 `AVDictionary`，`avformat_open_input` 的 `options` 参数再 forward 下去。

**Q2. `AVBufferPool` 是什么？和 `AVBufferRef` 什么关系？**
`AVBufferRef` 是"一块数据 + 引用计数"的最小单位；`AVBufferPool`（`libavutil/buffer.h:255`）是一个**复用池**：预分配一组同样 size 的 buffer，`av_buffer_pool_get` 返回一个 `AVBufferRef`，当 refcount 归零时并不真正 free，而是归还到 pool 的 freelist。音频 decoder 每帧产出的 PCM 都很大（1152×8ch×4B≈36KB），如果每帧都 malloc/free cache 会被打穿。decoder 内部的 `FramePool`（`libavcodec/internal.h:69`）就是 AVBufferPool 的应用。

**Q3. `AVCodecParameters` 和 `AVCodecContext` 为什么是两个结构体？**
历史原因：`AVCodecContext` 以前既表示"流的参数"又表示"解码器运行时状态"，导致 `AVStream::codec` 这个字段是一个 200 多字段的巨物，demuxer 只想描述流却被迫 alloc 一个 decoder-sized context。FFmpeg 3.1 拆分出 `AVCodecParameters`：只含描述流所需的静态信息（codec_id / sample_rate / channel_layout / extradata），不含 `priv_data` / `AVCodecInternal`。`AVStream::codecpar` 存这个轻量版本，用户调 `avcodec_parameters_to_context` 再倒进运行时 ctx。好处：remuxing 不需要 decoder、线程更安全、ABI 更稳定。

**Q4. `av_get_packet` 内部做什么？和 `av_new_packet + avio_read` 的区别？**
见 `libavformat/utils.c:98`。它做两件事：(1) 把 `pkt->pos` 置为当前 `avio_tell`（seek 时用）；(2) 调 `append_packet_chunked` 分块 `av_new_packet` / `av_grow_packet` + `avio_read`，直到读够 size 或遇到 EOF。比自己写 `av_new_packet(pkt, size); avio_read(pb, pkt->data, size)` 好在：short read 时自动截断（`pkt->size = 实际读到`），EOF 时清空 packet 避免脏数据，读失败会 set `AV_PKT_FLAG_CORRUPT`。MP3 demuxer 的 `mp3_read_packet` 就是直接用它。

**Q5. `AVCodec` vs `FFCodec`（`libavcodec/codec_internal.h:127`）——为什么要封装一层？**
`AVCodec` 是 public ABI（`libavcodec/codec.h`），字段能加不能删不能改意义。`FFCodec` 是 internal ABI：首字段是 `AVCodec p`（通过 `ffcodec()` 做 cast），后面跟 `caps_internal`、`cb_type`、`init/close`、union `{decode, receive_frame, encode, receive_packet}`、`priv_class`、`priv_data_size` 等 internal 字段。这样 codec 作者只暴露一个 `const AVCodec *` 给用户，但内部可以自由改 internal 字段，不破坏用户 ABI。`ffcodec(avctx->codec)->flush` 这种调用就是从 public cast 回 internal。

**Q6. `av_dict_set` 和 `av_opt_set` 的区别和使用场景。**
| 函数 | 作用对象 | 存储 | 典型场景 |
|---|---|---|---|
| `av_dict_set` | `AVDictionary *` | key→value 字符串表 | 打开前填"还不知道最终 ctx 在哪"的 options，如 `avformat_open_input(&fmt, url, NULL, &opts)` |
| `av_opt_set` | 任何含 `AVClass*` 的结构体 | 直接写入 struct 字段（通过 offset） | 打开后/alloc 后动态调参数，如 `av_opt_set(swr, "in_chlayout", ..., 0)` |

关键：`avformat_open_input` 消化 `AVDictionary` 时内部就是对每个 key 调 `av_opt_set`。open 后 dict 里**剩下的**就是"谁都不认识"的 key，常用来检测拼写错误。

**Q7. 为什么 `avformat_alloc_context` 没有后缀，但 `avcodec_alloc_context3` 有？**
FFmpeg 遵守 ABI/source 兼容规则：签名变了就改名字。`avcodec_alloc_context3` 的 `3` 表示它是第三版——第一版没参数，第二版加了 `AVCodec *`，第三版才变成现在的 `const AVCodec *codec`（const 化）。`avcodec_open2` 的 `2` 是同理（加了 `AVDictionary **options`）。`avformat_alloc_context` 一直没改签名所以没有后缀。面试规则记忆法：**后缀数字 = 历史版本数**，不是 API 等级。

**Q8. `avformat_network_init` 到底做了什么，为什么 HTTP 必须先调？**
见 `libavformat/utils.c:561`。它只做两件事：`ff_network_init()`（Windows 上调 `WSAStartup`；Unix 上基本 no-op）和 `ff_tls_init()`（初始化 OpenSSL/GnuTLS 的全局状态、线程锁回调）。不调的话 Windows 直接 socket 失败；Linux 上 plain HTTP 可能侥幸能工作但 HTTPS 一定崩在 TLS 全局 state 上。设计上它应该在进程启动时调一次、退出时配对 `avformat_network_deinit`。

**Q9. `AV_TIME_BASE` 和 `AVStream::time_base` 的关系？**
`AV_TIME_BASE = 1000000`（1μs），是 **`AVFormatContext::duration` / `AVFormatContext::start_time` 的单位**。但 `AVStream::time_base` 是每个流独立的有理数，packet 的 `pts/dts` 是这个单位。换算用 `av_rescale_q(ts, st->time_base, AV_TIME_BASE_Q)`。常见陷阱：用户拿 packet 的 pts 直接除以 `AV_TIME_BASE`，结果错成 1000000 倍。MP3 里 `st->time_base = {1, sample_rate}`（如 `{1, 44100}`），一帧 1152 samples 对应 pts=0, 1152, 2304…

**Q10. 为什么 FFmpeg 很多函数用 double-pointer 出参，例如 `avformat_open_input(AVFormatContext **ps, ...)`？**
三个原因：(1) **自动分配**：如果 `*ps == NULL`，函数内部 `avformat_alloc_context`；非 NULL 则用用户预 alloc 的（留给用户配 options / callback）。(2) **失败时置 NULL**：函数失败会 `av_freep(ps)`，调用者不用再手动 free，防止 double-free。(3) **API 可演进**：内部可以替换 ctx 对象而不影响调用者持有的指针。同样模式见 `av_frame_free(AVFrame **)`、`avformat_close_input(AVFormatContext **)`。

---

### 3B. libavformat 深度

**Q11. `raw_packet_buffer`（`libavformat/demux.c:606` 附近）的生命周期是什么？**
这是**流探测阶段**的缓冲。`ff_read_packet` 读到的 packet 如果对应流的 `sti->request_probe > 0`（还没确定 codec），就 `avpriv_packet_list_put` 到 `fci->raw_packet_buffer`，并累加 `raw_packet_buffer_size`。探测完成后，这些缓冲的 packet 会被 pop 出来当作真正的 packet 返回。**什么时候清**：探测成功、流关闭、或者 `raw_packet_buffer_size >= probesize`（见 `demux.c:456`）强制结束探测。**何时会"泄漏"**：probesize 设得太大而 codec 又识别不出来时，这里会吃掉几十 MB 内存直到 probesize 耗尽。

**Q12. `AVParser` 是什么，为什么 MP3 demuxer 需要 `AVSTREAM_PARSE_FULL_RAW`？**
`AVParser`（`libavcodec/avcodec.h:2832` 附近）是一个**比特流切帧器**：从字节流里找到帧边界、输出一个个完整的 codec frame，并顺带解析出 `pts/dts/duration`。MP3 demuxer 的 `mp3_read_packet` 一次读 1024 字节原始数据，根本不对齐到 MP3 帧，需要 parser 再切成 `576*N` samples 的 MP3 frame 才能送 decoder。`AVSTREAM_PARSE_FULL_RAW`（`libavformat/mp3dec.c:392`）表示"原始比特流 + 完整 parse"，即 demuxer 不做任何帧切分，**完全委托给 parser**。对比 `AVSTREAM_PARSE_HEADERS`（只解析头）、`AVSTREAM_PARSE_NONE`（信任 demuxer 自己已经切好帧）。

**Q13. `av_parser_parse2` 的 `in_data/in_size/out_data/out_size` 四参数语义？**
签名：`int av_parser_parse2(AVCodecParserContext *s, AVCodecContext *avctx, uint8_t **poutbuf, int *poutbuf_size, const uint8_t *buf, int buf_size, int64_t pts, int64_t dts, int64_t pos)`。语义：把 `buf/buf_size` 作为**增量输入**喂进 parser；parser 内部累积字节，一旦凑够一帧就通过 `poutbuf/poutbuf_size` 输出，否则 `*poutbuf_size = 0` 表示"还差数据，继续喂"。返回值是**本次消耗的 input 字节数**（可能小于 buf_size）。调用方要在循环里 `buf += consumed; buf_size -= consumed;` 直到喂完。注意：`*poutbuf` 要么指向 parser 内部缓冲、要么直接指向 input——**调用方不 free**。

**Q14. AAC 的三种封装：ADTS vs LATM vs MP4 box 有什么区别？**
| 封装 | 头结构 | 用途 | 识别 |
|---|---|---|---|
| ADTS | 每帧前 7/9 字节 sync word `0xFFF` + profile/sample_rate/channels | `.aac` 裸流、DVB/广播 | `aac` demuxer |
| LATM | MPEG-4 Part 3 定义，支持多子流复用、配置和数据分离 | MPEG-TS 里的 AAC（LOAS/LATM）、RTP | `loas` / `latm` demuxer |
| MP4 box | 没有逐帧 header，配置在 `esds` box 的 `AudioSpecificConfig`，data 放在 `mdat` | `.m4a` / `.mp4` | `mov` demuxer |

区别落到 decoder：ADTS 每帧自带采样率和声道，decoder 可以 on-the-fly 切换；MP4 的 AAC 需要 demuxer 把 `AudioSpecificConfig` 放进 `extradata` 让 decoder 在 `init` 时吃。

**Q15. `AVIOContext` 的内部 buffer 和 `short_seek_threshold` 是怎么回事？**
`AVIOContext` 持有一个 `buffer` + `buf_ptr` + `buf_end`（默认 32KB），所有 `avio_r8` / `avio_rb32` 从这里取，耗尽时调 `read_packet` 回调补。**`short_seek_threshold`**（`libavformat/aviobuf.c:83`，默认 4KB）：如果 `avio_seek` 的目标位置在当前 buffer **之后但相距 < short_seek**，直接连读丢弃数据而不 seek 底层。原因：HTTP range seek 要关连接再新建（或发新的 `Range` 头），代价远大于读几 KB。对 MP3 seek 尤其重要——Xing TOC 是 100 个点，相邻点很近，短 seek 能大幅降低 HTTP 请求数。

**Q16. `avio_seek` 对不可 seek 流返回什么？demuxer 如何应对？**
底层 protocol 的 `seek` 回调返回负数（通常 `AVERROR(EINVAL)` 或 `AVERROR(ENOSYS)`），`avio_seek` 透传。demuxer 的对策：(1) 如果 seek 方向是 forward 且距离小，走 `short_seek` 路径连读丢弃；(2) 否则让 `AVFormatContext::pb->seekable = 0`，上层 `avformat_seek_file` 会直接 fail 而不试图去 seek。`AVIO_SEEKABLE_NORMAL`（bit 0）和 `AVIO_SEEKABLE_TIME`（bit 1，较少用）是 seekable 的两种语义：前者是字节 seek 能力，后者是时间戳 seek 能力。

**Q17. `avio_feof` 为什么不可靠？**
见 `libavformat/aviobuf.c:349`：
```c
int avio_feof(AVIOContext *s) {
    if (s->eof_reached) { s->eof_reached = 0; fill_buffer(s); }
    return s->eof_reached;
}
```
它**先清除 EOF 再试着读一次**，才返回结果。对于网络流这是合理的（可能只是暂时没数据），但意味着 `avio_feof == 0` **不能证明还有数据**，只能证明"上一次读失败不是真 EOF"。判断流结束的正确方式：看 `av_read_frame` 返回 `AVERROR_EOF`。同时还有个坑：它不区分"真 EOF"和"网络错误"，两者都会置 `eof_reached`。

**Q18. `avformat_find_stream_info` 的 `probesize` 内存开销是什么？**
`probesize` 是**"允许 demuxer 在探测阶段吃掉多少字节"的上限**。这些字节会被累积到 `raw_packet_buffer`（`demux.c:456`），直到 `raw_packet_buffer_size >= probesize` 才停止探测。默认 5MB。对音频影响：MP3 普通文件 50KB 就能识别完，没问题；但做网络探测时每个流都吃 5MB，10 路并发就是 50MB 常驻，内存敏感场景需要调小到 64KB。副作用：太小可能识别不出 AAC profile/声道数，上层拿到 `codec_id == AAC` 但 `sample_rate == 0`。

**Q19. `AVFMT_FLAG_DISCARD_CORRUPT` 和 `AV_PKT_FLAG_CORRUPT` 的关系。**
demuxer 读到错误数据时会 set `pkt->flags |= AV_PKT_FLAG_CORRUPT`（例如 `av_get_packet` short read 后自己置位）。`av_read_frame` 的默认行为是照样返回给用户；但如果用户在 `AVFormatContext::flags` 里设了 `AVFMT_FLAG_DISCARD_CORRUPT`，framework 会直接丢弃这种 packet 继续读下一个。典型使用：直播流容错——宁可跳过几十毫秒也别把 decoder 喂出 `AVERROR_INVALIDDATA`。代价：丢弃的 packet 可能是关键帧（但对音频无 B-frame 影响小）。

**Q20. MP3 demuxer 给 parser 的 `AVSTREAM_PARSE_FULL_RAW` 具体要 parser 做什么？**
四件事：(1) 在字节流里**寻找帧同步** `0xFFE`（11-bit sync word + MPEG version + layer 约束）；(2) 解析 frame header 算出 `frame_size`（跟 bitrate、sample_rate、padding 有关），把整帧切出来；(3) 填 `AVPacket::pts` = 累计 samples / sample_rate（单位是 stream time_base）；(4) 填 `AVPacket::duration = spf`（MPEG1 L3 是 1152）。因为 demuxer 自己 `mp3_read_packet` 返回的 1024 字节不一定对齐到帧边界，parser 必须做 re-sync，这就是为什么不能用 `AVSTREAM_PARSE_NONE`。

**Q21. 为什么 MP3 demuxer 的 `mp3_read_packet`（`libavformat/mp3dec.c:449`）一次读 1024 字节而不是读一帧？**
两个原因：(1) **简单**——不用在 demuxer 里实现 MP3 frame header 解析，全交给 parser；(2) **边界灵活**——MP3 帧 size 从 ~100B（8kbps）到 ~1440B（320kbps）不等，读固定 1024 既不至于太小浪费 syscall 也不会大到跨太多帧。parser 会把残留字节缓存下一次拼接。对比 `aac` demuxer，它也是读固定 chunk 交给 parser。**设计哲学：demuxer 只负责"字节流来源"，parser 负责"帧对齐"**。

**Q22. ID3v2 和 ID3v1 在 MP3 文件里的位置和优先级？**
| Tag | 位置 | 大小 | 字符集 |
|---|---|---|---|
| ID3v2 | 文件**开头** | 可变（header 10B + 长度字段） | UTF-16/UTF-8 |
| ID3v1 | 文件**末尾 128 字节** | 固定 128B | Latin-1 |

读取顺序：`mp3_read_header` 先 `ff_id3v2_read_dict`（文件头），然后 seek 到 `-128` 读 ID3v1。`AVDictionary` 合并时 ID3v2 优先（因为后写入的 key 默认不覆盖）。ID3v2 能表达 APIC 封面图、多语言 comment、replay gain；ID3v1 只能存 30 字节的 title/artist/album。现代文件两者并存是为了兼容老播放器。

**Q23. `AVStream::duration` 和 `AVFormatContext::duration` 的单位和计算？**
| 字段 | 单位 | 来源 |
|---|---|---|
| `AVStream::duration` | `AVStream::time_base` | demuxer 从 container 读出或从首末帧推算 |
| `AVFormatContext::duration` | `AV_TIME_BASE`（μs）| 所有 stream duration 的最大值（rescale 到 μs） |

MP3 的计算特别：Xing/Info 有 `frames` 字段就 `duration = frames * 1152 / sample_rate`；没有就靠 `filesize / bitrate` 估算（VBR 会偏差）。实际代码见 `mp3_parse_info_tag`（`libavformat/mp3dec.c:175`）。start_time 也会被 encoder delay 修正（`start_skip_samples`）。

**Q24. `avformat_seek_file` 的 4 个时间参数什么意思？**
签名：`avformat_seek_file(s, stream_index, min_ts, ts, max_ts, flags)`。语义：**"请 seek 到 [min_ts, max_ts] 范围内最接近 ts 的位置"**。旧 API `av_seek_frame` 只有一个 ts + `BACKWARD` flag，表达力不够。4 参数能精准控制：
- `min_ts = INT64_MIN, max_ts = ts` → 往前 seek 到不超过 ts 的位置（等价旧 BACKWARD）
- `min_ts = ts, max_ts = INT64_MAX` → 往后 seek
- `min_ts = max_ts = ts` → 严格对齐（demuxer 找不到就 fail）

`stream_index == -1` 表示让 demuxer 自选基准流（通常是 "default" 流），时间基是 `AV_TIME_BASE_Q`。

**Q25. `AVSEEK_FLAG_ANY` / `BACKWARD` / `FRAME` / `BYTE` 对音频 seek 的影响。**
| flag | 含义 | 音频语义 |
|---|---|---|
| `BACKWARD` | seek 到不晚于目标的关键帧 | MP3 每帧都是关键帧，退化为"最近的帧" |
| `ANY` | 允许 seek 到非关键帧 | 对音频无意义（全是关键帧）|
| `FRAME` | `ts` 单位是帧号而不是 time_base | 少用，需 demuxer 支持 |
| `BYTE` | `ts` 单位是字节 offset | 破坏时间戳但对没有 index 的流是唯一可靠方式 |

对 MP3：`BACKWARD` 是默认，seek 走 Xing TOC 或线性扫描到最近帧；无 Xing TOC 时会用 `ts × filesize / duration` 粗估 + `check` 函数往回找 sync。

**Q26. `AVFMT_FLAG_NOBUFFER` 对音频应用的实际影响。**
作用是让 demuxer 不做 `packet_buffer` 的乱序缓冲，读到什么立刻返回。对视频直播有意义（可以降延迟，但会失去 GENPTS 能力）；对音频影响几乎为零，因为音频没有 B-frame，packet_buffer 基本不起作用。但它还会禁用 `avformat_find_stream_info` 的某些内部缓冲，**代价**是 `sample_rate`/`channels` 信息可能探测不完整。低延迟播放常见组合：`AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS`。

**Q27. `av_read_frame` 循环里为什么不能复用同一个 `AVPacket *` 不 unref？**
`av_read_frame` 返回的 packet 可能：(1) 从 demuxer 直接 fill 了 `data/buf`（引用 demuxer 内部 buffer）；(2) 从 `packet_buffer` pop 出来；(3) 指向一个新 allocate 的 ref。这些情况下 `buf` 都是 `AVBufferRef *`，持有对底层数据的引用。**不 unref 就 leak**：下一次调用 `av_read_frame` 会覆盖 `pkt->buf`，上次的 ref 失去了最后的持有者。正确姿势：
```c
while (av_read_frame(fmt, pkt) >= 0) {
    ...process pkt...
    av_packet_unref(pkt);  // MUST
}
```
循环外 `av_packet_free(&pkt)`。

**Q28. `ff_id3v2_read_dict` 和 `ff_id3v2_parse_apic` 分工。**
`ff_id3v2_read_dict`（`libavformat/id3v2.c:1143`）：第一遍扫描 ID3v2，把所有 text frame（TIT2/TPE1/TALB 等）塞进 `AVDictionary`（metadata），把 APIC/GEOB 等二进制 frame 存入 `ID3v2ExtraMeta` 链表——**不立即 alloc AVStream**，因为 demuxer 还没准备好流。`ff_id3v2_parse_apic`（`id3v2.c:1171`）：第二遍，从 extra_meta 里挑出 APIC 封面图，为它**创建一个 attached pic 流**（`AVStream` + `disposition = AV_DISPOSITION_ATTACHED_PIC`），把 JPEG/PNG 数据放进 `AVStream::attached_pic`。分两步的原因：`read_dict` 阶段 demuxer 可能还在判断是否接受这个文件，不能提前创建 stream。

**Q29. Xing 头和 Info 头的区别？为什么 CBR 用 "Info" 这个魔术字？**
两者结构完全一样，只是 magic 不同：
- `Xing`（`libavformat/mp3dec.c:184`）：VBR 文件，带 frames/size/TOC 信息，播放器需要用 TOC 做 seek
- `Info`：CBR 文件，表示"我有这个头但 bitrate 恒定，seek 用 bitrate × time 直接算即可，TOC 可忽略"

区分 magic 让老播放器快速决定策略：看到 `Info` 可以跳过 TOC 解析。代码里 `mp3->is_cbr = (v == MKBETAG('I','n','f','o'))`。值得注意：LAME 加的"Info"头紧跟在**第一个静音帧的 side info 区域**，所以第一个"帧"实际上不含音频，是个 placeholder。

**Q30. LAME header 里的 `encoder_delay` 和 `end_padding` 是怎么用的？**
`libavformat/mp3dec.c:254-272`：LAME 头里 24 bit 保存了 `start_pad`（高 12 bit，编码器前面塞的静音 sample 数，典型 576）和 `end_pad`（低 12 bit，文件尾部为对齐 MDCT 窗塞的 padding）。demuxer 把它们写进：
```c
sti->start_skip_samples      = mp3->start_pad + 528 + 1;
sti->first_discard_sample    = -mp3->end_pad + 528 + 1 + frames*1152;
sti->last_discard_sample     = frames*1152;
```
`+ 528 + 1` 是 MP3 decoder 的**固有 IMDCT delay**（跟编码器无关的解码器侧残余）。libavcodec 的 `skip_samples` 机制会根据这些字段在输出 AVFrame 前裁掉前后无效 PCM，让 gapless playback 成为可能。

**Q31. ReplayGain tag 是存在哪里的？`METADATA_REPLAYGAIN` 是怎么被 demuxer 提取的？**
三种位置：(1) LAME header 的 `peak` / `r_gain` / `a_gain` 字段（`mp3dec.c:223-245`），被直接提取成 `AVPacketSideData` / `AVStream::side_data`，类型 `AV_PKT_DATA_REPLAYGAIN`；(2) ID3v2 的 `TXXX:REPLAYGAIN_TRACK_GAIN` 等自定义文本 frame，走 `ff_id3v2_read_dict` → 放进 `AVDictionary`；(3) APE tag（MP3 文件末尾，ID3v1 之前），少用。demuxer 不做数值统一，上层（filter `volume`/`loudnorm`）自己 parse dict 里的字符串或读 side_data 里的结构体。

**Q32. `avformat_close_input` 一定会 close `pb` 吗？**
**不一定**（`libavformat/demux.c:388`）：
```c
if ((s->iformat && strcmp(s->iformat->name, "image2") 
     && s->iformat->flags & AVFMT_NOFILE) ||
    (s->flags & AVFMT_FLAG_CUSTOM_IO))
    pb = NULL;  // 不关
```
两种情况不 close：(1) `AVFMT_NOFILE` 格式（rtmp/rtp 自己管 socket），`pb` 本就是 NULL；(2) `AVFMT_FLAG_CUSTOM_IO` 用户自己 alloc 的 `pb`，framework 不能动。用户用自定义 IO 时**必须自己 `avio_context_free(&pb)`**，否则泄漏。这是 custom IO 最常见的漏洞点。

**Q33. `AVFormatContext::url` 不等同于 `filename` 参数？**
`filename` 是字段名，`url` 是现在的名字（8 char limit 的历史遗留改掉了）。两者都是 `avformat_open_input` 传入字符串的**拷贝**，但 url 可能被 protocol 层改写：例如 `concat:` 协议会把整个 concat list 存进来；HTTP redirect 后 `url` 会更新为最终地址。对比：`filename` 字段在新版里被 deprecated，建议一律读 `url`。注意 `url` 的 ownership 归 `AVFormatContext`，`avformat_close_input` 会 free 它，用户别自己去 free。

**Q34. `AVProbeData::mime_type` 从哪里来？HTTP 和本地文件的区别？**
`AVProbeData`（`libavformat/avformat.h:451`）有 `mime_type` 字段用来帮助 probe 选对 demuxer。来源：
- **HTTP 场景**：`http.c` 解析 response 头的 `Content-Type`，传到 `ff_probe_input_format3`。比如服务端返回 `audio/mpeg`，probe 直接优先挑 MP3 demuxer
- **本地文件**：`mime_type = NULL`，完全靠二进制 magic + 扩展名 score 竞争

好处：某些格式 magic 很弱（比如 LATM 裸流），mime_type 能一锤定音。坏处：服务端 `Content-Type` 不靠谱是常态（`application/octet-stream`），所以 FFmpeg 把它当**提示**而不是**权威**，最终还是用 score 选。

**Q35. `AVFMT_NOFILE` 格式（rtmp、rtp）的 `AVIOContext` 为什么是 NULL？**
这类协议**自己管连接**，不需要通过 `AVIOContext` 的 read/write/seek 三回调抽象。rtmp 是 TCP 长连接 + AMF 消息，rtp 是 UDP 包边界保留，都不匹配字节流模型。demuxer 内部直接 `ffurl_open` 拿到自己的 `URLContext *` 存进 `priv_data`。所以 `fmt->pb == NULL`，`avformat_close_input` 检查 `AVFMT_NOFILE` 时就跳过了 `ff_format_io_close`。这也解释了为什么 CLI 的 `-protocol_whitelist` 要同时列 `rtp,udp`——demuxer 层是 rtp，底下的 udp socket 是独立的一层 protocol。

---

### 3C. libavcodec 通用

**Q36. `FFCodec::init_static_data` 的作用（举 MP3 Huffman 表预计算为例）。**
MP3 decoder 需要 32 组 Huffman 表（`ff_mpa_huffcodes`），解码时用 VLC 查表加速。但 VLC table 本身也要先"计算"出来（`ff_vlc_init` 根据 code/bits 数组生成查找表）。如果每次 `avcodec_open2` 都重建，既浪费 CPU 也不必要。FFmpeg 的模式：codec 注册一个 `init_static_once`（`AVOnce`），在 `init` 里调 `ff_thread_once(&init_static_once, mp3_init_static)` 保证全进程只算一次。例子见 `libavcodec/atrac3.c:854` 的 `atrac3_init_static_data`、`libavcodec/qdmc.c:166`。优点：多线程安全、懒惰初始化（不用的 codec 不算表）。

**Q37. `FFCodec::caps_internal` 里的 `FF_CODEC_CAP_INIT_CLEANUP`（`libavcodec/codec_internal.h:42`）是什么？**
声明"这个 codec 的 `init` 回调如果中途失败，会自己 cleanup 已分配的资源"。没有这个 flag 的话，framework 在 `init` 返回错误后会**主动调** `codec->close` 来收尾；有这个 flag framework 就**不调** close——因为 codec 承诺自己清干净了。它是一个反向标志：默认行为（framework 兜底调 close）更稳，但某些 codec 的 close 无法处理"只初始化了一半"的状态（会在 `av_freep` NULL 之外的字段 crash），所以作者选择 INIT_CLEANUP 来宣告"别帮我，我自己行"。查一个 codec 是否 set 这个 flag 能推断它 init 的容错风格。

**Q38. `avctx->internal` 指针指向什么（`AVCodecInternal` 里有什么）？**
`libavcodec/internal.h:49` 定义。关键字段：
- `in_pkt` / `last_pkt_props` / `buffer_pkt` / `buffer_frame`：push/pull API 的"暂存一个 packet/frame"槽位（实现 send_packet 内部预拉一帧的优化）
- `draining` / `draining_done`：flush 状态机
- `skip_samples` / `pad_samples`：音频 gapless 相关
- `pool`（`FramePool`）：per-codec 的 AVBufferPool
- `bsf`：自动插入的 bitstream filter（如 AAC raw → ADTS）
- `is_copy` / `is_frame_mt`：frame-threading worker 标记
- `needs_close`：配合 INIT_CLEANUP

它是"public ctx 字段不够放，又不想破 ABI"的避难所。用户不应直接访问。

**Q39. `avcodec_find_decoder(AV_CODEC_ID_MP3)` vs `avcodec_find_decoder_by_name("mp3float")`？**
`find_decoder(id)` 按 codec_id 查询，返回**第一个 match 的 decoder**——注册表里顺序决定。MP3 的注册顺序使得 `mp3float`（float 版）排在前面，所以 by id 默认得到 `mp3float`。`find_decoder_by_name("mp3")` 则显式要定点版（`MP3Decoder`，输出 `s16p`）；`find_decoder_by_name("mp3float")` 显式要 float 版（输出 `fltp`）。场景：嵌入式无 FPU 会用 `mp3`；PC/手机一律 `mp3float`。两者共享 `mpegaudiodec_template.c`，只是模板参数不同。面试加分点：ffmpeg 的 `-codec:a mp3` CLI 参数走的就是 by_name。

**Q40. `avcodec_flush_buffers` 对音频 decoder 内部状态具体做了什么？**
见 `libavcodec/avcodec.c:381`。步骤：
1. 调 `ff_decode_flush_buffers(avctx)`：清空 `avci->in_pkt`、`last_pkt_props`，重置 `skip_samples/discard_samples` 计数器，清空 `bsf`（如果有）
2. `draining = 0; draining_done = 0`：把 drain 状态机回 IDLE
3. unref `buffer_frame` / `buffer_pkt`：丢弃没被 receive 的那一帧/那个包
4. 如果 codec 自己定义了 `->flush` 回调，调它——MP3 的 flush 会清零 `mdct_buf`（IMDCT overlap 残留），防止 seek 后的"旧声音泄漏到新位置"

**关键**：flush 后的第一帧，解码器处于**冷启动状态**，前半帧 IMDCT overlap 没有历史数据，会产生短暂 click/静音——这就是为什么 seek 完通常要丢弃第一帧或 fade in。视频 H.264 的 flush 还会额外清 DPB，音频 decoder 没这回事。

---

### 3C. libavcodec 通用机制（续）

**Q1. `decode_simple_internal` 和 `ff_decode_receive_frame_internal` 的分工？**
两层包装，对应解码器的两种回调类型。`decode_simple_internal`（`libavcodec/decode.c:417`）只服务 `FF_CODEC_CB_TYPE_DECODE` 老式解码器——从 `avci->in_pkt` 取数据，调 `codec->cb.decode(...)`，处理 `got_frame_ptr` 和"一个 packet 多次调用消化"的循环，内部做 `discard_samples`。`ff_decode_receive_frame_internal`（`libavcodec/decode.c:614`）是**分发层**：若 `cb_type == FF_CODEC_CB_TYPE_RECEIVE_FRAME`，直接调 `codec->cb.receive_frame(...)`（新式 push/pull 解码器，如 AAC 的一些路径、FFV1、硬件解码）；否则循环调 `decode_simple_receive_frame → decode_simple_internal` 直到拿到一帧或 EAGAIN。所以 **simple 是"老式解码器的适配器"，receive_frame_internal 是"统一入口"**。

**Q2. `FF_CODEC_CB_TYPE_DECODE` vs `FF_CODEC_CB_TYPE_RECEIVE_FRAME` 对音频解码器的含义？**
- `FF_CODEC_CB_TYPE_DECODE`：老式"每 packet 消化几字节、输出 0 或 1 帧"的回调签名 `int (*decode)(AVCodecContext*, AVFrame*, int *got_frame_ptr, AVPacket*)`。MP3 (`libavcodec/mpegaudiodec_template.c`)、PCM、vorbis 等绝大多数音频都是这一类。
- `FF_CODEC_CB_TYPE_RECEIVE_FRAME`：新式"解码器自己管理内部状态，被动被 pull"的签名 `int (*receive_frame)(AVCodecContext*, AVFrame*)`。hw 解码器、部分并行化解码器走这条。
对音频而言，绝大多数 codec 停留在 DECODE 类型，因为 1 packet → 1 frame 的映射天然契合 simple 语义；只有 AAC USAC 这类需要内部缓冲多个帧的实现会用 RECEIVE_FRAME。

**Q3. `avci->in_pkt` / `avci->buffer_frame` 是干啥的？**
（题目原文"`compat_decode_consumed`"在当前 tree 已不存在——相关功能被 `avci->in_pkt` 吸收。）`AVCodecInternal::in_pkt` 是 decoder 内部拥有的 packet 存储，`send_packet` 把用户传入的 packet 内容转到这里（零拷贝 ref）；`decode_simple_internal` 通过 `ff_decode_get_packet(avctx, pkt)` 取用，解码后若 `consumed < pkt->size` 则 `pkt->data += consumed; pkt->size -= consumed;`（`libavcodec/decode.c:497-508`），实现"一个 packet 分多次喂给 decoder"。这就是老式 API `int consumed` 返回值的语义在新架构下的承载。

**Q4. `AVCodecContext::extradata` 对 AAC / Opus / FLAC 各意味着什么？**
- **AAC**：`AudioSpecificConfig`（2–5 字节），含 profile、sampling frequency index、channel config、SBR/PS 标志。MP4/mkv 解封装时放进 extradata；ADTS 流则无 extradata，header 字段嵌在每帧里。
- **Opus**：`OpusHead`（19 字节），含 version、channel count、preskip（`initial_padding`）、input sample rate、output gain、mapping family。preskip 字段会被 decoder 翻译成 `AV_FRAME_DATA_SKIP_SAMPLES`。
- **FLAC**：`fLaC` + STREAMINFO block（38 字节左右），含 min/max blocksize、min/max framesize、sample rate、channels、bps、total samples、MD5。
三者都是 decoder 必须拿到才能 `avcodec_open2` 成功的前置配置信息。

**Q5. `AVCodecContext::frame_size` 为什么 MP3 是 1152，AAC-LC 是 1024？**
这是 codec 规范固化的 MDCT 窗口大小：
- MP3 (MPEG-1 Layer 3)：2 个 granule × 576 = 1152 samples（`libavcodec/mpegaudio.h:37` 的 `MPA_FRAME_SIZE 1152`）。MPEG-2/2.5 LSF 每帧只有 1 granule = 576。
- AAC-LC：1 个 long window = 1024 samples；short window block 为 8×128 = 1024。AAC-LD 是 512，AAC-ELD 是 480/512。
`frame_size` 对音频 encoder 是"除最后一帧外必须恰好送这么多 sample"的约束。

**Q6. `AVCodecContext::delay` 和 `initial_padding` 的区别？**
- `initial_padding`（encode 侧）：encoder 为对齐 MDCT 而**在流开头塞入**的静音样本数。LAME 写 iTunSMPB / LAME tag 就是记这个。
- `delay`（encode 侧视频用，decode 侧两者都设）：decoder 从收到第一个 packet 到输出第一帧有效 PCM 之间的**内部延迟样本数**。对音频意味着 seek 后需要额外预热这么多样本才能输出无失真的 PCM。
`libavcodec/avcodec.h:583` 的注释明确区分：音频 encoding 用 `initial_padding`，decoding 用 `delay`。数值上二者对同一 codec 通常相等，但语义不同——一个是"bitstream 里藏了多少静音"，一个是"解码器自身的记忆长度"。

**Q7. `AV_FRAME_DATA_SKIP_SAMPLES` 的结构？**
`av_frame_new_side_data(frame, AV_FRAME_DATA_SKIP_SAMPLES, 10)`——10 字节：
```
bytes 0..3  : uint32 LE, 从帧头裁掉多少样本（skip）
bytes 4..7  : uint32 LE, 从帧尾裁掉多少样本（discard）
byte  8     : uint8, skip 原因
byte  9     : uint8, discard 原因
```
见 `libavcodec/decode.c:340` 的 `av_frame_new_side_data(frame, AV_FRAME_DATA_SKIP_SAMPLES, 10)` 和其后 `AV_WL32(side->data, avci->skip_samples); AV_WL32(side->data + 4, discard_padding);`。decoder 用来告诉下游"这帧前 N 个 / 后 M 个样本是 encoder delay / end padding，展示前请裁掉"。

**Q8. 为什么 `avcodec_parameters_from_context` 和 `_to_context` 是两个函数而不是 swap？**
因为 `AVCodecParameters` 是 `AVCodecContext` 的**子集**——只含"描述流所必需"的字段（codec_id、profile、extradata、音频参数、视频参数），不含 decoder 运行时状态（内部缓冲、线程、side data）。swap 语义会要求两个结构对称可互换，但 context 比 params 多出几十个字段，swap 会丢失或污染运行时状态。两个函数明确方向：
- `from_context`：把 decoder/encoder 的**最终**配置快照到 params（用于写 AVStream）。
- `to_context`：从 demuxer 读到的 params 配置一个新 context（用于开 decoder）。
避免把 "运行中的 context" 和 "静态描述" 搞混。

**Q9. `AVCodecContext::pkt_timebase` 的作用？**
decoder 需要知道"进来的 packet 的 pts/dts 是什么时间基"才能把它正确地 rescale 到 frame 的输出 pts。`pkt_timebase`（`libavcodec/avcodec.h:550`）就是用户（通常是调用方从 `AVStream::time_base` 复制过来）告诉 decoder 的这个时间基。decoder 内部做 `guess_correct_pts`、做 `SKIP_SAMPLES` 时的 pts 补偿（`libavcodec/decode.c:369` 的 `av_rescale_q(avci->skip_samples, (AVRational){1, avctx->sample_rate}, avctx->pkt_timebase)`）都要用它。**不设 pkt_timebase 时 decoder 无法正确换算 skip_samples 导致的 pts 偏移。**

**Q10. `AVCodecContext::time_base` 对 decoder 为什么是 read-only？**
注释明确写 "encoding: MUST be set by user / decoding: unused"（`libavcodec/avcodec.h:541`）。decoder 不读它因为：
1. decoder 的输入是 packet，packet 的时间基由 `pkt_timebase` 指定；
2. decoder 的输出是 frame，frame 的"时间基"概念通过 `sample_rate` 直接表达（音频）或由调用方自行维护（视频）。
历史上有些代码错误地把 `AVStream::time_base` 赋给 `codec_ctx->time_base`，这不会报错但毫无意义。唯一合法的写入场景是 encoder 告诉 libavcodec "我送的 frame pts 用这个时间基"。

**Q11. `ff_thread_get_buffer` 对音频为什么是空操作？**
`ff_thread_get_buffer` 是 frame-threaded 解码下分配 frame buffer 的接口，要做"主线程分配 + worker 解码 + 帧间依赖同步"。音频解码**从不启用 frame threading**（FF_THREAD_FRAME 对音频无意义：一帧 20ms，跨帧无依赖可言，并行只增加延迟）。所以对音频路径，`ff_thread_get_buffer` 直接退化为 `ff_get_buffer → avcodec_default_get_buffer2`，不走任何跨线程握手。音频 decoder 看到的 frame 永远是本线程直接从 `av_buffer_pool` 拿的。

**Q12. `avcodec_open2` 内部做了什么？**
主要步骤（`libavcodec/avcodec.c`）：
1. `av_opt_set_defaults` 填 AVOption 默认值；
2. 分配 `AVCodecInternal`（含 `in_pkt`、`buffer_frame`、`last_pkt_props`）；
3. 校验参数合法（sample_rate、ch_layout、pix_fmt 等）；
4. 如是硬件解码：`ff_decode_get_hw_frames_ctx`、`setup_hwaccel`；
5. 调 `codec->init(avctx)` 让 codec 自己的 private context 初始化（例如 MP3 的 VLC 表、mdct lut）；
6. 若 codec 需要 `AVCodec::caps_internal & FF_CODEC_CAP_INIT_CLEANUP`，init 失败会自动调 close；
7. 重置 `avci->draining = 0; draining_done = 0;`、清 `in_pkt`、准备接收第一个 packet。
对音频 decoder 来说关键是第 5 步（建表）和第 7 步（状态机归零）。

**Q13. `AVCodecContext::codec_descriptor` 何时被填？**
在 `avcodec_open2` 内部，根据 `avctx->codec_id` 调 `avcodec_descriptor_get(codec_id)` 填入，指向静态的 `AVCodecDescriptor` 表项（`libavcodec/codec_desc.c`）。`AVCodecDescriptor` 描述"这个 codec 的类别/名字/能力标志"，是**与具体实现无关**的元信息（MP3 有 mp3 和 mp3float 两个 decoder，但共享同一个 descriptor）。外部通过 `avcodec_descriptor_get_by_name("mp3")` 可以在不依赖具体 decoder 的情况下查询 codec 信息。

**Q14. `DecodeContext` 和 `AVCodecInternal` 的包含关系？**
`DecodeContext` 是 `AVCodecInternal` 内嵌的子结构，专门存 decode-only 的状态。`libavcodec/decode.c` 顶部有 `static DecodeContext *decode_ctx(AVCodecInternal *avci) { return (DecodeContext *)avci; }`——二者在内存上**同首地址**。`AVCodecInternal` 定义在 `libavcodec/internal.h`，公共字段（in_pkt、buffer_frame、draining、draining_done、skip_samples 等）通过 avci 访问；decode-only 字段（`nb_draining_errors`、`initial_pict_type`、`intra_only_flag`）通过 decode_ctx() 把它 cast 成 `DecodeContext` 访问。encoder 路径同理有 `EncodeContext`。

**Q15. `avcodec_default_get_buffer2` 对音频 frame 的分配策略？**
对音频分支（`libavcodec/get_buffer.c`）：
1. 算 `buf_size = av_samples_get_buffer_size(NULL, channels, nb_samples, sample_fmt, align)`，其中 `align = 0` 走 default 对齐（通常 32 字节以便 SIMD）；
2. 从一个 per-AVCodecContext 的 `AVBufferPool` 取 buffer（`avci->pool`）——同一 decoder 连续解码会**复用**同一片内存；
3. 调 `av_samples_fill_arrays(frame->extended_data, &frame->linesize[0], buf->data, channels, nb_samples, sample_fmt, align)` 填指针；
4. `frame->buf[0] = buf_ref`，所有 plane 共享这一个 ref。
planar 格式 N 通道也只占 `buf[0]` 一个 ref——内存是**一次 malloc 内部切片**（见原文 Q33）。

**Q16. `AVCodecContext::opaque_ref` 的用途？**
用户在 `send_packet` 时通过 `AVPacket::opaque_ref` 传入任意引用计数数据（例如业务层的 seq_id 结构），decoder 会把它**原样转移**到对应输出 `AVFrame::opaque_ref`。这样用户不需要维护"packet→frame"的外挂映射表就能跨越 decoder 内部缓冲追踪每帧的上下文。典型场景：HLS 播放器给每个 packet 打上 segment_id，解出来的 frame 要能回溯到原 segment 做无缝切换。注释见 `libavcodec/avcodec.h:248`。

**Q17. `avcodec_receive_frame` 返回 `AVERROR_EOF` 之后再调会怎样？**
继续返回 `AVERROR_EOF`。`decode_simple_internal` 开头就检查 `if (avci->draining_done) return AVERROR_EOF;`（`libavcodec/decode.c:435`），一旦 drain 完成这个标志不会自动复位。要让 decoder 再次可用必须 `avcodec_flush_buffers(ctx)`——它会 `avci->draining = 0; avci->draining_done = 0;` 并 call codec 的 flush 回调清内部状态（MP3 里就是 `memset(ctx->mdct_buf, 0, ...); ctx->last_buf_size = 0;`，`libavcodec/mpegaudiodec_template.c:1632`）。

**Q18. `pkt_dts` 在 audio frame 里的含义（音频没有 B 帧为什么还有 dts）？**
`AVFrame::pkt_dts` 是 decoder 从"生成这一帧所消化的 packet" 的 dts 拷贝过来的。音频确实没有 B 帧，pts == dts 恒成立，但这个字段仍然保留是因为：
1. **API 对称性**：decode.c 里 `if (!(codec->caps_internal & FF_CODEC_CAP_SETS_PKT_DTS)) frame->pkt_dts = pkt->dts;`（`libavcodec/decode.c:448`）对音视频一视同仁；
2. **兜底 pts**：demuxer 可能只给 dts（MPEG-PS）或只给 pts，调用方用 `guess_correct_pts` 在两者间选；
3. **调试**：对齐 packet 和 frame 时 dts 是稳定的"进入时刻"标记。
音频代码一般直接用 `frame->pts`，但 `pkt_dts` 在 pts 缺失时是兜底。

**Q19. `AVCodecContext::skip_frame` 的 `AVDISCARD_*` 对音频影响？**
`AVDISCARD_NONE/DEFAULT/NONREF/BIDIR/NONINTRA/NONKEY/ALL`——原本是视频用的（跳 B 帧、跳 non-key）。对音频：
- 所有音频帧都是独立"key frame"，没有 ref/bidir/nonintra 的概念；
- `AVDISCARD_NONKEY` 和 `AVDISCARD_ALL` 在音频上会导致**所有帧都被跳过**——decoder 内部仍会解，但输出 frame 打上 `AV_FRAME_FLAG_DISCARD`，`decode_simple_internal` 里就地 `av_frame_unref` 然后返回 EAGAIN 继续下一帧；
- 实际效果等同于"快速消化 packet 但不输出 PCM"，常用于 seek 后需要预热 MDCT 状态但不需要输出的场景。
但绝大多数播放器不用这个——音频 seek 直接 flush decoder 即可。

**Q20. 为什么 `AVCodec::decode` 回调签名有 `int *got_frame_ptr` 这种老式参数？**
历史包袱。FFmpeg 0.x 时代的 API 是 `avcodec_decode_audio4(ctx, frame, &got_frame, pkt)`，外部 API 以此为形状；内部 decoder 回调就照着长。到了 send_packet / receive_frame 时代外部 API 换了，但**内部回调签名没动**——只需要在 `decode_simple_internal` 这个 wrapper 里把 `got_frame` 翻译成 EAGAIN 即可（`libavcodec/decode.c:457`）。改内部回调要动 400+ decoder 文件，收益不大，所以保留。新式解码器走 `FF_CODEC_CB_TYPE_RECEIVE_FRAME` 这条干净的新路径。

---

### 3D. MP3 内部机制

**Q21. MP3 frame header 32 位的字段布局？**
按高位到低位（`libavcodec/mpegaudiodecheader.c:34` 解析）：

| bits | 字段 | 含义 |
|---|---|---|
| 31..21 | sync | 11 位 `0x7FF`（严格 MPEG-1 要求 12 位 `0xFFF`，MPEG-2/2.5 降为 11 位） |
| 20 | version_msb | 0=MPEG-2.5, 1=MPEG-1/2 |
| 19 | version_lsb | 配合 bit20：11=MPEG-1, 10=MPEG-2, 00=MPEG-2.5, 01=保留 |
| 18..17 | layer | 11=Layer1, 10=Layer2, 01=Layer3, 00=保留 |
| 16 | protection | 0=有 CRC, 1=无 CRC（取反后存 `error_protection`） |
| 15..12 | bitrate_index | 查 `ff_mpa_bitrate_tab[lsf][layer-1][idx]` |
| 11..10 | sample_rate_index | 查 `ff_mpa_freq_tab[idx]`，再按 lsf/mpeg25 右移 |
| 9 | padding | 是否多 1 字节（或 Layer1 多 4 字节） |
| 8 | private_bit | 私有标志，decoder 忽略 |
| 7..6 | channel_mode | 00=Stereo, 01=JointStereo, 10=DualChannel, 11=Mono |
| 5..4 | mode_ext | 仅 JointStereo 有意义：bit5=MS, bit4=I-stereo |
| 3 | copyright | 版权标志 |
| 2 | original | 原版/拷贝 |
| 1..0 | emphasis | 00=none, 01=50/15 µs, 10=reserved, 11=CCITT J.17 |

**Q22. `MPA_MAX_CODED_FRAME_SIZE` 是多少，为什么？**
`#define MPA_MAX_CODED_FRAME_SIZE 1792`（`libavcodec/mpegaudio.h:40`）。MP3 最大帧字节数可能是 Layer 2 @ 384 kbps @ 32 kHz：`frame_size = 144000 * 384 / 32000 = 1728`，加 padding 1 字节 = 1729；再加上一些对齐裕量凑到 1792。这个数被用作解码器临时 buffer 的静态大小上限（`mpegaudiodec_template.c:1661` 的 `if (len > MPA_MAX_CODED_FRAME_SIZE) len = MPA_MAX_CODED_FRAME_SIZE;` 截断保护），也被 MP2 encoder (`libtwolame.c:115`) 用作 `av_new_packet` 的大小。

**Q23. MP3 的 granule 是什么，为什么 MPEG-1 一帧有 2 个、MPEG-2 只有 1 个？**
granule 是 Layer 3 的基本处理单位——576 samples（= 18 subband × 32 samples）。MPEG-1 Layer 3 一帧含 2 个 granule = 1152 samples；MPEG-2/2.5 LSF（Low Sample Frequency）为了在低采样率下保持每帧时长合理，一帧只有 1 granule = 576 samples。源码里 `nb_granules = s->lsf ? 1 : 2`（`mpegaudiodec_template.c:1225`）。每个 granule 有独立的 part2_3_length、big_values、huffman 表选择、block_type——真正的解码循环是按 granule 迭代的。

**Q24. bit reservoir 机制：`main_data_begin` 和 `last_buf` 为什么存在？**
MP3 允许一帧的 main_data（scalefactor + huffman 比特流）**跨越帧边界**——即当前帧的 main_data 可能有前 N 字节位于**前几帧**的尾部。`main_data_begin` 是 9 位字段（MPEG-1）或 8 位字段（MPEG-2），表示"本帧 main_data 相对 sync 字往回偏移多少字节"，最大 511 字节（MPEG-1）。这让 encoder 可以把复杂段的比特预支到简单段的空位里，在恒定帧尺寸下实现 VBR 质量。
decoder 要做这事就必须**保留前一帧的尾部比特**——`uint8_t last_buf[LAST_BUF_SIZE]`（`mpegaudiodec_template.c:79`），每次解完一帧把剩余比特 memmove 到 `last_buf` 开头（`mpegaudiodec_template.c:1500`），下帧解码时先从 `last_buf` 消化 `main_data_begin` 字节再接当前帧数据（第 1306–1316 行）。`LAST_BUF_SIZE = 2 * BACKSTEP_SIZE + EXTRABYTES`。

**Q25. Huffman 表有几张，如何选？**
Layer 3 定义了 **34 张** Huffman 表（table 0、1–16、24）用于 big_values 区，其中 15 对是实际用的（0 是"全零"哑表，某些下标保留）。加上 2 张 quad 表用于 count1 区（signed quad values）。一个 granule 的频谱被分成 3 个 region（region0/1/2），每个 region 独立选一张表——共 3 个 5-bit `table_select[]` 字段（`mpegaudiodec_template.c:66`）。count1 区有 1-bit `count1table_select`。encoder 按失真最小化选表，decoder 只按字段查 `ff_huff_vlc[l]`（`mpegaudiodec_template.c:783`）即可。

**Q26. scalefactor 的 LSF 和 normal 区别？**
scalefactor 是"每个 scalefactor band 的量化步长"。
- **MPEG-1（normal）**：每个 granule 独立传 21 组 long scalefactor 或 12 组 short scalefactor；相邻 granule 可用 `scfsi`（scale factor selection info，4 位，`mpegaudiodec_template.c:1236`）决定是否从前一 granule 复制。
- **MPEG-2 LSF**：去掉 scfsi（因为只有 1 granule），改用**分层的 scalefactor 压缩表**——`slen[0..3]` 四组通过查表 `ff_lsf_nsf_table` 决定每组几位；intensity stereo 还有独立的 LSF scalefactor 表。
两者解码走不同分支（`mpegaudiodec_template.c` 的 `exponents_from_scale_factors` vs `exponents_from_scale_factors_lsf`）。

**Q27. Stereo / JointStereo (MS/IS) / DualChannel / Mono 四种声道模式的解码路径差异？**
`mode` 字段（header bit 7..6）：
- **Stereo (00)**：两个独立声道，各自完整走 huffman + requantize + imdct，输出 L/R。
- **JointStereo (01)**：看 `mode_ext`。MS stereo（bit5=1）：比特流里存的是 M=(L+R)/√2、S=(L-R)/√2，解码后做 `compute_stereo`（`mpegaudiodec_template.c:1456`）反变换回 L/R。IS stereo（bit4=1）：高频段只传 mid 的绝对值和一个 position，L/R 按 position 分配能量——只在高频省码率。两者可同时启用。
- **DualChannel (10)**：两个**独立的 mono 流**（比如双语），decoder 按 stereo 走但下游语义不同。对解码路径 = Stereo。
- **Mono (11)**：`nb_channels = 1`，只有一条路径，`last_buf` 和 `mdct_buf` 只用一个 channel slot。
MS/IS 反变换位置：`compute_stereo` 在 huffman + requantize 之后、imdct 之前。

**Q28. 为什么 MP3 decoder 维护 `mdct_buf[MPA_MAX_CHANNELS][SBLIMIT * 18]`？**
MDCT/IMDCT 是 **50% overlap** 的 lapped transform：第 n 帧 IMDCT 输出的后半部分要和第 n+1 帧 IMDCT 输出的前半部分相加才是最终 PCM。decoder 必须保存**前一帧 IMDCT 尾部**供本帧叠加。`mdct_buf[MPA_MAX_CHANNELS][SBLIMIT * 18]`（`mpegaudiodec_template.c:89`，`SBLIMIT = 32`，每 subband 18 samples tail = 576 samples 一个 channel 的尾部）就是这个环形状态。flush 时 `memset(ctx->mdct_buf, 0, sizeof(ctx->mdct_buf))`（第 1632 行）清零——这也是 seek 后头一帧可能有瞬态噪声的原因。

**Q29. `ff_mpa_synth_filter` 的 polyphase 合成滤波器？**
MP3 的最后一级是**多相合成滤波器组**（polyphase synthesis filterbank）：把 32 个 subband 的时域样本（每 subband 18 个）通过 32 个子带滤波器合成回 1152 个 PCM 样本。`ff_mpa_synth_filter`（`libavcodec/mpegaudiodsp.c` 的各 SIMD 优化版本）实现这个滤波：
1. IDCT32 变换把 32 subband 值域转到时域；
2. 和一个 512-tap 的 window 卷积（`ff_mpa_synth_window`）；
3. 维护一个 512 样本的环形 history buffer（`synth_buf`）。
这一级是 MP3 解码最吃 CPU 的部分，几乎所有架构都有 SIMD 版本（SSE/AVX/NEON/VSX）。

**Q30. MP3 的 CBR 和 VBR 区别，decoder 怎么处理？**
- CBR（Constant Bit Rate）：每帧 bitrate_index 相同，`frame_size` 字节数基本恒定（只有 padding 在浮动）。
- VBR（Variable Bit Rate）：每帧 bitrate_index 可以不同，帧字节大小逐帧变化；配合 bit reservoir，encoder 能在复杂段临时"借"更多比特。
**decoder 本质不区分**——每帧都读 header → 从 header 算 frame_size → 消化 frame_size 字节。CBR/VBR 对 decoder 完全透明。区别只在 demuxer 层：CBR 可以 `duration = file_size * 8 / bitrate` 精确算时长，VBR 必须靠 Xing/VBRI 头或扫描全文件才能知道总时长。

**Q31. Xing 头如何告诉 demuxer 总帧数？**
Xing 头位于**第一个 MP3 帧的 side_info 区之后**（MPEG-1 stereo 在偏移 36 字节，MPEG-1 mono 21 字节，MPEG-2 stereo 21 字节，MPEG-2 mono 13 字节），4 字节 magic `"Xing"` 或 `"Info"`（后者表示 CBR-with-TOC）。后接 4 字节 flags，根据 flags 依次有：
- `frames`（4 字节）：总帧数；
- `bytes`（4 字节）：总字节数；
- `TOC[100]`：100 个百分位的字节偏移表；
- `quality`（4 字节）：VBR scale。
demuxer 读到 Xing 后用 `frames * 1152 / sample_rate` 算总时长，用 TOC 做 seek。这是给 demuxer 的元信息，decoder 不关心——Xing 所在的那帧通常是"假帧"（全静音或 granule 数据无效），解码出来是静音。

**Q32. LAME 头的 `encoder_delay` 和 `end_padding` 怎么被转成 `AV_FRAME_DATA_SKIP_SAMPLES`？**
LAME 头紧跟在 Xing 头之后（偏移 +120 字节处），长 36 字节，含版本串、revision、vbr method、**encoder delay（12 bit）+ end padding（12 bit）**（打包成 3 字节）、以及 replaygain、mp3gain、preset 等。MP3 demuxer（`libavformat/mp3dec.c`）解析出这两个值后塞进 `AVStream::start_skip_samples` 和 `AVStream::first_discard_sample / last_discard_sample`，demuxer 在送出 packet 时给第一个 packet 打 `AV_PKT_DATA_SKIP_SAMPLES` side data，decoder 里 `decode.c:327` 的 `av_frame_get_side_data(frame, AV_FRAME_DATA_SKIP_SAMPLES)` 读出 `avci->skip_samples` 和 `discard_padding`，在输出 PCM 时把前 delay 样本和后 padding 样本裁掉（`decode.c:358-383`）。

**Q33. MP3 解码的 7 个阶段？**
按 `decode_frame_mp3on4` / `mp_decode_layer3` 调用顺序（`libavcodec/mpegaudiodec_template.c`）：
1. **header 解析**：32 位 header → sample_rate, bitrate, mode, frame_size；
2. **side_info 解析**：main_data_begin、part2_3_length、big_values、table_select、scfsi、block_type、subblock_gain 等（~17/32 字节，看 mode）；
3. **bit reservoir 重建**：从 `last_buf` 拼接 main_data，`init_get_bits` 重新指向拼好的比特流；
4. **scalefactor 解码**：按 slen[0..3] 读每 sfb 的 scalefactor，应用 pretab 和 preemphasis；
5. **Huffman 解码**：按 region 选表解 big_values，再解 count1 区和 rzero 区得到 576 个频谱系数；
6. **requantize & reorder & stereo processing**：`x^(4/3)` 反量化 + short block reorder + alias reduction + MS/IS stereo 反变换；
7. **IMDCT + overlap-add + polyphase synthesis**：`compute_imdct` 做 18/6-point IMDCT → 和 `mdct_buf` 叠加 → `ff_mpa_synth_filter` 合成 1152 PCM。
最后写入 AVFrame。

**Q34. MP3 的 "free format" 是什么？**
header 里 `bitrate_index == 0` 的合法情况（不是非法），表示"此流使用非标准 bitrate"。此时 header 不告诉你 frame_size，decoder 必须自己扫后续字节直到下一个 sync 才能知道本帧多长。极少见（某些老 encoder 或硬件 codec），绝大多数播放器不完整支持。FFmpeg 的 `mpegaudiodecheader.c:96` 遇到 `bitrate_index == 0` 返回 1（表示 "frame_size 未知"），由调用方后续处理。

**Q35. MPEG-2.5 Layer 3 和 MPEG-1 Layer 3 的兼容性？**
MPEG-2.5 是 **Fraunhofer 的非标准扩展**（ISO 并未正式批准），用来支持 8/11.025/12 kHz 超低采样率。header 标识通过 bit20=0 + bit19=1 识别（见 `mpegaudiodecheader.c:44`）。比特流结构基本兼容 MPEG-2 LSF（1 granule、576 samples/frame），只是采样率表和 sample_rate_index 不同（`ff_mpa_freq_tab[idx] >> (lsf + mpeg25)`）。严格的 MPEG-1 decoder 不认 MPEG-2.5 sync 字（因为 2.5 的 sync 只有 11 位 `0x7FF` 而 MPEG-1 要求 12 位 `0xFFF`），但 FFmpeg 和多数现代 decoder 都支持。

**Q36. MP3 padding bit 的作用？**
44.1 kHz @ 128 kbps 的理想帧大小是 `144000 * 128 / 44100 = 417.9...` 字节——**不是整数**。解决办法：平均每 N 帧里有 9 帧是 417 字节，1 帧是 418 字节，平均值恰好 417.9。header 里 1 位的 **padding 字段** 指示本帧是否多一字节（Layer1 是多 4 字节）。encoder 用简单的累加器：每帧累加 `frac = frame_size * 1000`，当累加溢出 `sample_rate` 时这帧标 padding=1 并扣除。见 `mpegaudiodecheader.c:87` 的 `frame_size = (frame_size * 144000) / sample_rate; frame_size += padding;`。

**Q37. `mp3float` 内部用什么类型做 IMDCT？**
`mp3float` 走 `libavcodec/mpegaudiodec_float.c`，它 `#define USE_FLOATS 1` 再 `#include "mpegaudiodec_template.c"`。模板里 `INTFLOAT` 宏根据 `USE_FLOATS` 展开成 `float` 或 `int32_t`；`OUT_INT` 对应 `float` 或 `int16_t`。mp3float 的 `mdct_buf[MPA_MAX_CHANNELS][SBLIMIT * 18]` 是 `float`，IMDCT 全程浮点，最终输出 `fltp`。另一个 `mp3`（定点，`mpegaudiodec_fixed.c`）用 `int32_t` 做 IMDCT，输出 `s16p`。**两套实现共享源码但实例化出不同的符号**，这是 C 版 "template" 的典型用法。

**Q38. MP3 里的 `private_bits` 是什么？**
两处：
1. header 第 8 位的 `private_bit`——1 位，规范保留给应用层私用，decoder 完全忽略。
2. side_info 里 MPEG-1 有 5 位 `private_bits`（mono 时）或 3 位（stereo 时），MPEG-2 有 1 位或 2 位；同样保留给私用。
encoder 通常置 0，FFmpeg decoder 读过即丢。历史上有过尝试用 private_bits 夹带元数据的工程，但没成规模。面试时知道"private_bits 是规范留的逃生舱口、decoder 忽略"即可。

**Q39. MP3 decoder 如何处理 emphasis 字段？**
**不处理。** emphasis 是 header bit 1..0 指示模拟时代的 pre-emphasis 类型（50/15 µs 或 J.17 或无）——encoder 标记"我加了高频预加重，请你做对应的 de-emphasis"。`libavcodec/mpegaudiodecheader.c:70` 明确是注释：`//emphasis = header & 3;` —— FFmpeg **连读都不读**，直接丢弃。原因：
1. 实际存在的 MP3 几乎全是 emphasis=0；
2. 真要做 de-emphasis 需要额外一级 IIR 滤波，工程代价不值得；
3. 误设 emphasis 的文件反而会在真 decoder 里被错误补偿。
所以这是个规范保留但实现层完全忽视的字段。

**Q40. 为什么 MP3 seek 到随机位置后头两帧可能有噪声？**
两个独立来源，叠加：
1. **bit reservoir 依赖**：当前帧的 main_data 可能有一部分位于前 1–2 帧的尾部（`main_data_begin` 最大 511 字节，约覆盖 2 帧）。seek 后 `last_buf_size = 0`（`mpegaudiodec_template.c:1633`），decoder 拿不到这部分比特，huffman 解码会从错位开始——整帧频谱系数废掉，输出垃圾或静音。
2. **MDCT overlap-add 冷启动**：IMDCT 需要前一帧的 `mdct_buf` 尾部做叠加。seek 后 `memset(ctx->mdct_buf, 0, ...)`，第一帧的前半部分是和"零"叠加——频谱系数即便正确，时域包络也不对，听感是瞬态 click 或高频振铃。
要缓解：向前多解码 2–3 帧作为"预热"再从目标位置开始输出（就是 `AVCodecContext::delay` 字段在文档里告诉你的事）。专业播放器（如 rubberband、foobar2000）都这么做；简单播放器直接接受头两帧有噪。

---

### 3E. AAC（LC / HE / USAC）

**Q1. AAC-LC / HE-AAC v1 / HE-AAC v2 的比特率、采样率与频率特征是什么？**

| 变种 | AOT | 工具链 | 典型比特率 | 输出 PCM 频率 | 典型听感 |
|---|---|---|---|---|---|
| AAC-LC | 2 (AOT_AAC_LC) | MDCT + Huffman，无 SBR/PS | 96-256 kbps | 44.1/48 kHz | CD 透明度 ≈ 128 kbps |
| HE-AAC v1 (AAC+) | 5 (AOT_SBR) | LC + SBR（频域重建） | 32-96 kbps | 编码 22.05 解 44.1 | 24 kbps 接近 LC 64 kbps |
| HE-AAC v2 (eAAC+) | 29 (AOT_PS) | LC + SBR + PS（参数立体声） | 16-48 kbps | 同上，立体声参数化 | 专做极低码率广播/流媒体 |

核心观察：HE v1/v2 都是**两段采样率**模式——核心 AAC 运行在"半速率"（例如 22.05 kHz），SBR 把高频补回来，最终 PCM 是全速率。`libavcodec/aac/aacdec.c` 在首次遇到 SBR payload 时把 `avctx->profile` 从 LC 改写成 `AV_PROFILE_AAC_HE` 或 `AV_PROFILE_AAC_HE_V2`（见下题 Q6）。

---

**Q2. AAC frame 基本是 1024 样本，但 SBR 生效时为什么输出 2048？**

- 核心 AAC 的 MDCT 窗长决定一个 raw_data_block 输出 **1024 samples**（短窗 8×128 = 1024；`frame_length_short=1` 时 960；USAC low-delay 可以是 768）。见 `libavcodec/aac/aacdec.c:2174` `int samples = m4ac->frame_length_short ? 960 : 1024;`。
- SBR 的 QMF analysis/synthesis bank 做 **2× upsampling**：核心解出 1024 个低频样本，SBR 把它们升采样到 2048 并补齐高频包络。
- 所以一个 HE-AAC 包在 48 kHz 输出下是 2048 samples，但对应的 packet duration 仍然是 "1 core frame"；`ac->oc[1].m4ac.ext_sample_rate` 记录外部（输出）采样率，`sample_rate` 记录核心采样率。这也是 `AV_CODEC_CAP_CHANNEL_CONF` + "implicit SBR signaling" 需要特殊处理的根源——第一帧前 decoder 还不知道是不是 SBR，必须能"升级"。

---

**Q3. ADTS 头 7 字节（带 CRC 则 9 字节）里有哪些关键字段？**

参考 `libavcodec/adts_header.c:30` 的 `ff_adts_header_parse`：

```
syncword           12 bit  = 0xFFF
ID                  1 bit  (0=MPEG-4, 1=MPEG-2)
layer               2 bit  = 0
protection_absent   1 bit  (0 时尾部带 2 字节 CRC)
profile_objecttype  2 bit  (AOT - 1，所以只能表示 LC/Main/SSR/LTP)
sampling_freq_idx   4 bit  (查 ff_mpeg4audio_sample_rates[16])
private_bit         1 bit
channel_config      3 bit  (查 ff_mpeg4audio_channels[15])
original/copy       1 bit
home                1 bit
copyright_id_bit    1 bit
copyright_id_start  1 bit
aac_frame_length   13 bit  (整个 ADTS 帧字节数，包括头)
adts_buffer_fullness 11 bit
num_raw_data_blocks 2 bit  (实际帧数 = 该字段 + 1)
```

注意点：
1. `profile` 只有 2 bit，**没法表示 SBR/PS**——HE-AAC 在 ADTS 层只能声明自己是 LC，decoder 靠 payload 里的 `sbr_extension_data()` 自动"升级"。
2. `aac_frame_length` 包含头本身，所以连续解析时 `ptr += frame_length` 即可找到下一个 syncword。
3. `num_raw_data_blocks` 理论上允许一个 ADTS 包含多个 raw data block，但实际主流编码器都是 1。

---

**Q4. LATM/LOAS 封装和 ADTS 有什么区别？**

| 维度 | ADTS | LATM/LOAS |
|---|---|---|
| 定义处 | MPEG-2 AAC 附录 A | MPEG-4 Systems Part 3 |
| sync | 12 bit `0xFFF`（每帧） | LOAS 同步字 `0x2B7`（11 bit，见 `libavformat/loasdec.c:29`） |
| 配置信息 | **内嵌在每个头**（profile/sr/chan） | **配置和载荷分离**：`StreamMuxConfig` 描述，之后每帧只带 payload |
| 多子流 | 不支持 | 支持多 program / multiplex |
| FFmpeg demuxer | `libavformat/aacdec.c`（`adts_aac_probe`） | `libavformat/loasdec.c` → `AV_CODEC_ID_AAC_LATM` |
| 典型应用 | 广播/播客/移动应用 | DVB-T/ISDB 系统、RTP |

`aacdec_latm.h` 是 AAC decoder 的 LATM 变体入口，在 `libavcodec/aac/aacdec_latm.h:347` 注册。LATM 的 payload 里包含 AudioSpecificConfig，所以 decoder 即使没有外部 extradata 也能工作——这是 LATM 对广播"随进入"场景的核心优势。

---

**Q5. AudioSpecificConfig (ASC) 是什么？放在 extradata 里的是什么？**

ASC 是 MPEG-4 Systems 定义的"描述一个 AAC 流怎么解码"的紧致比特流。FFmpeg 把它放到 `AVCodecContext::extradata`，`libavcodec/mpeg4audio.c:92` 的 `ff_mpeg4audio_get_config_gb` 负责解析：

```
GetAudioObjectType()        5 bit（若 == 31，再读 6 bit + 32 → AOT_ESCAPE）
samplingFrequencyIndex      4 bit (0xF = 显式 24 bit sampleRate)
channelConfiguration        4 bit
if (AOT == SBR or PS):
    extSamplingFrequencyIndex 4 bit
    AudioObjectType           5 bit（"核心"AOT）
GASpecificConfig:
    frameLengthFlag          1 bit (1 → 960/480 samples per frame)
    dependsOnCoreCoder       1 bit
    extensionFlag            1 bit
    ...
```

在 MP4 里 ASC 来自 `esds` box（见 Q23），在 Matroska 里来自 `CodecPrivate`，在 ADTS 流里**没有**——所以 ADTS 解码路径靠第一帧重建一个隐式的 ASC。

---

**Q6. `AVCodecContext::profile` 里 AAC profile 值是多少？**

`libavcodec/defs.h:68-76`：

```c
#define AV_PROFILE_AAC_MAIN         0
#define AV_PROFILE_AAC_LOW          1
#define AV_PROFILE_AAC_SSR          2
#define AV_PROFILE_AAC_LTP          3
#define AV_PROFILE_AAC_HE           4   // SBR
#define AV_PROFILE_AAC_LD          22
#define AV_PROFILE_AAC_HE_V2       28   // SBR + PS
#define AV_PROFILE_AAC_ELD         38
#define AV_PROFILE_AAC_USAC        41
```

规律：**AV_PROFILE_AAC_* = AOT - 1**（见 `libavcodec/aac/aacdec.c:2186` 的注释与 `ac->avctx->profile = aot - 1;`）。所以 AOT_AAC_LC=2 → profile=1，AOT_SBR=5 → profile=4，AOT_PS=29 → profile=28，AOT_USAC=42 → profile=41。HE/HE_V2 的 profile 不是在 ASC 读完就定的，而是在解到 SBR payload 时"升级"——`libavcodec/aac/aacdec.c:1972` 把 profile 改成 HE_V2，:1977 改成 HE。

---

**Q7. SBR (Spectral Band Replication) 的原理是什么？**

SBR 的哲学：人耳对高频细节不敏感，高频可以"从低频拷贝 + 参数修正"出来。

```
编码端:                           解码端:
全频 PCM                         SBR payload
   │                                │
   ▼                                ▼
分成 QMF 64 子带               ┌─ 核心 AAC 解 低频 QMF
   │                           │
   ├─ 低频 → 核心 AAC 编码     │ SBR 工具:
   └─ 高频 → 提取包络/        └─→ HF generator (从低频复制/patch)
            噪声/音调参数         包络调整 (envelope)
                                   噪声注入 (noise floors)
                                   音调合成 (sinusoids)
                                   QMF synthesis → 全频 PCM
```

在 FFmpeg 里：`libavcodec/aacsbr_template.c` 是主路径，`sbr_decode_extension` 在 `libavcodec/aac/aacdec.c:1980` 被调用。QMF bank 是 64-channel cosine-modulated filterbank，analysis/synthesis 是对称的。注意 SBR 每帧做的是"包络系数"而不是"频谱系数"，带来压缩比提升的同时引入 delay。

---

**Q8. PS (Parametric Stereo) 的原理是什么？**

PS = 把立体声信号**压成一个单声道 + 几个参数**。编码端混成 mono，decoder 用参数恢复立体声：

- **IID** (Inter-channel Intensity Difference) 左右声道强度差
- **ICC** (Inter-channel Cross-Correlation) 相关系数
- **IPD/OPD** (Inter-channel Phase Difference / Overall PD)

因为只传 mono 载荷，比特率可以非常低（16 kbps 左右）。代价：立体声场"人工化"。在 FFmpeg 里 PS 和 SBR 串联——`libavcodec/aac/aacdec.c:1969` 看到：

```c
if (m4ac->ps == -1 && ch_layout.nb_channels == 1) {
    m4ac->sbr = 1; m4ac->ps = 1;
    profile = AV_PROFILE_AAC_HE_V2;
}
```

PS 被发现时 decoder 会把 mono 流当立体声输出（`libavcodec/aac/aacdec_dsp_template.c:612` 的 `type == TYPE_SCE && ps == 1` 分支），并打印 "Treating HE-AAC mono as stereo."。

---

**Q9. AAC TNS (Temporal Noise Shaping) 是什么？为什么需要？**

TNS 是 "在频域做 LPC" 的奇技：

- 普通 MDCT 编码：量化噪声在整个 1024-sample 时域窗内均匀展开
- 问题：碰到瞬态（attack, transient），开头是一记鼓声、结尾是安静的衰减，前后时间位置的量化噪声会被拖成"preecho"——听感上像鼓声之前"啪"一下
- TNS 解法：对 MDCT 系数本身做 LPC（线性预测），残差量化后听起来噪声形状跟着信号包络走，noise 被"限制"在有能量的位置

`libavcodec/aac/aacdec.c:1600` 里 `tns_max_order` 的上限依 window type 而定（short 7, long 12, main 20）。TNS 在 encoder/decoder 都是可选工具，`libavcodec/aacsbr_template.c` 和 `aacdec.c` 都会在条件满足时走这个路径。

---

**Q10. AAC PNS (Perceptual Noise Substitution) 做什么？**

PNS 是另一个"比特省着用"的工具：某些 MDCT 子带本质上是"类噪声"的（例如掌声、海浪），编码器不传真系数，只传一个"能量"标量，decoder 本地生成 PRNG 噪声乘以这个能量。

- 省带宽：一个子带几百 bit → 几 bit
- 风险：如果 decoder 的 PRNG 不一致，立体声会失真
- AAC 规范要求两个声道使用相同 seed 以保持相位相关性

在 FFmpeg 里 PNS 由 scalefactor band 的特殊类型 `NOISE_BT` 标记，`libavcodec/aac/aacdec.c` 走 `decode_pulses` / noise path 分支。对应 encoder 见 `libavcodec/aaccoder.c`。

---

**Q11. AAC LTP (Long Term Prediction) 是什么？**

LTP 是 AAC Main 和 LTP profile 的工具（LC 没有）：在时域维护一个长时延迟缓冲（典型几百 ms），编码器对"和延迟缓冲相关"的信号做 pitch prediction，仅残差进入 MDCT。

代码印记：`libavcodec/aac/aacdec.c:2063` 的 `AOT_AAC_LTP` 分支；:1822 的 Main profile prediction 分支。LTP 在流行音乐上省不到多少（相对 LC 而言），所以 LC 才是主力 profile，LTP 几乎只出现在早期 MPEG-4 参考样例里。

---

**Q12. AAC block switching（LONG/SHORT/START/STOP window）的意义？**

AAC 的 MDCT 窗长是 1024 samples，变换分辨率 = 采样率/2048 ≈ 21 Hz @ 44.1 kHz——很好的频率分辨率，但时间分辨率差（46 ms），碰到瞬态会 pre-echo。解法：需要时切换成 **8 个 128-sample 短窗**。但 MDCT 是 lapped transform，不能"硬切"——必须靠过渡窗保持完美重建：

```
ONLY_LONG_SEQUENCE   : 1×1024 长窗 (稳态信号)
LONG_START_SEQUENCE  : 过渡：前半长窗形，后半短窗形
EIGHT_SHORT_SEQUENCE : 8×128 短窗 (瞬态)
LONG_STOP_SEQUENCE   : 过渡：前半短窗形，后半长窗形
```

见 `libavcodec/aac/aacdec_dsp_template.c:293-381`，`imdct_and_windowing` 根据 `ics->window_sequence[0]` 走四条路径。合法的状态转移是：LONG → LONG_START → SHORT → (SHORT... | LONG_STOP) → LONG；不能 LONG 直接跳 SHORT。

---

**Q13. AAC 的 `channel_configuration` 1-7 分别是什么？**

`libavcodec/mpeg4audio.c:59`：

```c
const uint8_t ff_mpeg4audio_channels[15] = {
    0,   // 0 = 定义在 PCE 里
    1,   // 1 = mono (C)
    2,   // 2 = stereo (L R)
    3,   // 3 = 3.0 (C L R)
    4,   // 4 = 4.0 (C L R Cs)
    5,   // 5 = 5.0 (C L R Ls Rs)
    6,   // 6 = 5.1 (C L R Ls Rs LFE)
    8,   // 7 = 7.1 (C Lc Rc L R Ls Rs LFE)
    0, 0, 0,
    7,   // 11 = 6.1 (C L R Ls Rs Cs LFE)
    8,   // 12 = 7.1 rear surround
    24,  // 13 = 22.2
    8,   // 14 = 7.1 front
};
```

config=0 是"看 PCE"（Program Config Element），即比特流自描述。在 FFmpeg 里这张表是只读共享表，ADTS/LATM/ASC 路径都查同一张。

---

**Q14. PCE 和 `channel_configuration=0` 有什么关系？**

当 ASC 的 `channelConfiguration == 0`，就意味着"**通道布局不是预定义的 1-7 之一**，去读 PCE"。PCE = Program Configuration Element，一个 raw_data_block 里的独立 syntactic element（`TYPE_PCE`, 见 `libavcodec/aac.h:49`），显式列出每个 element 的类型 (SCE/CPE/LFE/CCE) 和 tag。

在 `libavcodec/aac/aacdec.c:774` 附近的 `decode_pce` 逻辑里：读完 PCE 后 decoder 会生成一个 `layout_map[]` 数组，然后用 `ff_aac_output_configure` 建 channel layout。PCE 允许 AAC 表达任意非标准布局（比如 "5 个 SCE + 3 个 CPE + 2 个 LFE"），这是 AAC 支持 22.2 以上大阵列的方式。

---

**Q15. `libavcodec/aac/aacdec.c` 里 SCE / CPE / LFE / CCE 四种 element 类型分别是什么？**

`libavcodec/aac.h:43`：

```c
enum RawDataBlockType {
    TYPE_SCE,   // 0 Single Channel Element (单声道)
    TYPE_CPE,   // 1 Channel Pair Element (立体声对)
    TYPE_CCE,   // 2 Coupling Channel Element (下混参考通道)
    TYPE_LFE,   // 3 Low Frequency Effects
    TYPE_DSE,   // 4 Data Stream Element
    TYPE_PCE,   // 5 Program Config Element
    TYPE_FIL,   // 6 Fill Element (扩展数据，如 SBR payload)
    TYPE_END,   // 7 Block End marker
};
```

含义细节：
- **SCE**：一个声道。Mono = 1×SCE。5.1 的 Center 和 LFE 之外的某些 layout 也用 SCE。
- **CPE**：2 声道一对，可以启用 M/S stereo 和 intensity stereo（同一 MDCT 窗口共享）
- **CCE**：不直接对应输出声道，而是一个"下混参考流"——通过矩阵下混到最终 speaker
- **LFE**：LFE 和 SCE 解码路径几乎一样，但采样率带宽限制更严
- **FIL**：最重要——SBR payload 是作为 `FIL` 里的 `EXT_SBR_DATA` 进入 decoder 的

raw_data_block 就是这些 element 的连续序列，以 `TYPE_END` 结尾；见 `aacdec.c:2254` 的 `while ((elem_type = get_bits(gb, 3)) != TYPE_END)`。

---

**Q16. USAC 和 MPEG-D 的区别？USAC 又叫 xHE-AAC？**

- **USAC** (Unified Speech and Audio Coding) 是 MPEG-D 标准（ISO/IEC 23003-3），不是 MPEG-4 Audio 的扩展。
- 它被**集成进 MPEG-4 Audio 作为 AOT 42** (`AOT_USAC`)，这是它能"冒充"AAC 的原因。
- **xHE-AAC** = Extended HE-AAC = MPEG-D USAC profile 的别名，Android/Dolby/广播协议都用这个称呼。
- 关键创新：**同时包含 CELP (LPD, Linear Prediction Domain) 和 MDCT (FD, Frequency Domain) 两条路径，逐帧切换**。语音自动走 LPD（类似 AMR），音乐走 FD（类似 AAC）。
- FFmpeg 支持：`libavcodec/aac/aacdec_usac.c` + `aacdec_lpd.c` + `aacdec_usac_mps212.c`（MPS212 是立体声工具），profile 值 `AV_PROFILE_AAC_USAC = 41`。

USAC 还引入了 **SBR 改进版**（harmonic SBR/eSBR）、**MPEG Surround 212**（参数立体声的扩展）等。

---

**Q17. USAC 的 `ChannelElement` 为什么可以热切换？**

USAC 规范允许**同一个 element 在不同帧用不同 core_mode**：一帧是 LPD (ACELP/TCX)，下一帧是 FD (MDCT)。FFmpeg 的 `ChannelElement` 结构必须同时持有两套状态：

- `libavcodec/aac/aacdec.h:288` 附近的 `ChannelElement`：CPE 专属字段 + CCE 专属字段 + LPD 专属字段是 union/并存
- `libavcodec/aac/aacdec_usac.c` 在 parse element 时根据 `core_mode` bit 选走 LPD 还是 FD 路径
- `aacdec_lpd.c` 维护 LPD 的持久状态（pitch delay buffer 等）

OSS-Fuzz 历史上曝过多个相关 bug：当两帧之间 core_mode 切换时，如果 decoder 没正确清理另一路径的"陈旧状态"（比如 LPD 的 history buffer 残留），后续帧会读到未初始化内存或错位数据。FFmpeg 在 USAC 启用初期为此打过多次补丁，`ChannelElement` 的 init/reset 路径对 hot-switch 有特殊要求。

---

**Q18. 为什么 AAC 有 encoder delay（常见 2048 或 1024 samples）？**

MDCT 是 50% overlapped lapped transform：解码输出第 N 帧需要第 N-1 帧和第 N 帧的 IMDCT 结果叠加。这意味着：

1. **第 0 帧没有"前一帧"可以叠**，输出的前 512 sample 不是正确 PCM；
2. 编码器必须在前面塞**一整帧静音**，让 decoder 的 overlap-add 缓冲先充起来；
3. 典型 LC encoder delay = 2048 samples（两帧）或 1024（一帧，看编码器策略）；
4. HE-AAC 还要加 SBR QMF bank 的延迟（大约几百 sample）；
5. 这些 delay 信息被写进 MP4 `edts/elst`（edit list）或 iTunes `iTunSMPB` atom，FFmpeg 的 `AVStream::start_time` + `skip_samples` side data 负责在 decode 后裁掉。

对应 libopus 的 `pre_skip`（见 3F Q4）也是同样的机制。

---

**Q19. AAC decoder 的 `AV_CODEC_CAP_DELAY` 设 1 意味着什么？**

重要纠正：**FFmpeg 原生 AAC decoder 并没有设 `AV_CODEC_CAP_DELAY`**。`libavcodec/aac/aacdec.c:2583`：

```c
.p.capabilities  = AV_CODEC_CAP_CHANNEL_CONF | AV_CODEC_CAP_DR1,
```

原因：AAC 的 overlap-add delay 是 encoder 端引入的（见 Q18），decoder 每收到一个 packet 就能吐出一帧完整的 1024 samples，不需要 flush 就能拿到最后的数据。encoder delay 的裁剪通过 `skip_samples` side data 在 decoder 之外完成。

反例：如果哪天 decoder 内部真的缓冲了"下一帧"用于当前帧的预测（理论上 AAC LD/ELD 可能），那就需要 `CAP_DELAY`，调用方必须 `send_packet(NULL)` flush。目前的 LC/HE 路径不走这个。

---

**Q20. ADTS sync word 是 `0xFFF`（12 bit），和 MP3 的 `0xFFE`/`0xFFF` 如何区分？**

MP3 sync 字是 `0xFFE` 或 `0xFFF`（11 或 12 位全 1，看是否 MPEG-1 vs 2.5），ADTS 是 12 位 `0xFFF`。它们在首 12 位上有交集，区分靠"第 13 位往后的结构"：

- MP3 第 13 位是 "MPEG version"（00=2.5, 01=reserved, 10=2, 11=1）
- ADTS 第 13 位是 `ID`（MPEG-4/MPEG-2 AAC 分别是 0/1）
- MP3 Layer 字段（2 bit）= 01/10/11（不能是 00），ADTS Layer 字段 = 00
- 所以 ADTS 的"ID=0, layer=00"组合让 `(header & 0xFFF6) == 0xFFF0`（见 `libavformat/aacdec.c:51`）——ID bit 可以是 0/1 所以掩码里那位是 0，Layer 两位必须是 0 所以掩码保留那两位并要求其为 0

FFmpeg 的 ADTS probe (`libavformat/aacdec.c:35` `adts_aac_probe`) 用这个掩码做第一层过滤，然后按 `frame_length` 连续跳帧验证，连续多帧都命中才确信是 ADTS 而不是 MP3 或误命中。

---

**Q21. `aac_latm` demuxer 和 `aac` demuxer 的区别？**

| 项 | `ff_aac_demuxer` (`libavformat/aacdec.c`) | `ff_loas_demuxer` (`libavformat/loasdec.c`) |
|---|---|---|
| 封装 | ADTS (每帧 `0xFFF` 头) | LOAS AudioSyncStream (`0x2B7` 同步字) |
| Codec ID | `AV_CODEC_ID_AAC` | `AV_CODEC_ID_AAC_LATM` |
| time_base | `{1, 28224000}` (所有 ADTS 采样率 LCM) | `{1, 28224000}` 同 |
| 配置信息 | 从首帧头推断 | 从 LATM StreamMuxConfig 读取 |
| 应用 | 文件 `.aac`、iTunes Radio、ShoutCast | DVB-T/ISDB TS、RTP payload 14 |
| `need_parsing` | `AVSTREAM_PARSE_FULL_RAW` | `AVSTREAM_PARSE_FULL_RAW` |
| ID3v2 支持 | 是 (`FF_INFMT_FLAG_ID3V2_AUTO`) | 否 |

注意 **codec ID 不同**：`AAC` 解码器和 `AAC_LATM` 解码器在 FFmpeg 里是两个不同的 `FFCodec` 注册点（后者在 `libavcodec/aac/aacdec_latm.h:347`），因为 LATM 的 bitstream 结构根本不是 raw AAC——必须先解 LATM header 再 feed 到 AAC 核心。

---

**Q22. AAC 的 `sampling_frequency_index` 为什么是索引而不是直接的采样率？**

比特节省 + 硬件查表。4 bit 可以表示 16 个值，其中 13 个是预定义采样率：

`libavcodec/mpeg4audio_sample_rates.h:30`：

```c
const int ff_mpeg4audio_sample_rates[16] = {
    96000, 88200, 64000, 48000, 44100, 32000,
    24000, 22050, 16000, 12000, 11025, 8000, 7350
};  // 索引 13/14/15 保留，最后一个 0 给 encoder 遍历终止
```

对应关系：`0x0=96000, 0x1=88200, ... 0x4=44100, 0x5=32000, 0x8=16000, 0xB=8000, 0xC=7350`。索引 `0xF` 是"escape"——后面接 24 bit 的显式采样率（`ff_mpeg4audio_get_config_gb` 的 `get_sample_rate`）。好处：常见流 4 bit 就搞定；坏处：不能用任意采样率（只有 13 个档位）。LCM = 28224000 正好是这 13 个采样率的最小公倍数，这就是 FFmpeg 里把 AAC stream time_base 设成 `{1,28224000}` 的原因。

---

**Q23. MP4 里的 AAC `esds` box 是什么？**

`esds` = ES Descriptor box，是 MP4 (ISOBMFF) 用来放 MPEG-4 Systems 对象描述符的容器。对 AAC 而言它里面嵌套：

```
esds (FullBox, 4 bytes version/flags)
  └─ ES_Descriptor (tag 0x03)           ← MP4ESDescrTag
       ES_ID, flags...
       └─ DecoderConfigDescriptor (tag 0x04)  ← MP4DecConfigDescrTag
            objectTypeIndication, streamType,
            bufferSizeDB, maxBitrate, avgBitrate
            └─ DecoderSpecificInfo (tag 0x05)  ← MP4DecSpecificDescrTag
                 ← 这里的 raw bytes 就是 AudioSpecificConfig
            └─ SLConfigDescriptor (tag 0x06)
```

见 `libavformat/mov_esds.c:23` 的 `ff_mov_read_esds`，tag 常量定义在 `libavformat/isom.h:398`：

```c
#define MP4ESDescrTag           0x03
#define MP4DecConfigDescrTag    0x04
#define MP4DecSpecificDescrTag  0x05
```

`ff_mp4_read_dec_config_descr` 把 DSI 的字节 memcpy 到 `AVCodecContext::extradata`——这就是 MP4 里 AAC 的 ASC 来源。Length 字段是 iBMFF 风格的"escape coding"（1-4 字节，每字节最高位为 continuation bit），解析见 `ff_mp4_read_descr`。

---

**Q24. SBR 的 "implicit signaling" 和 "explicit signaling" 是什么？**

两种在 ASC 层声明 SBR 存在与否的方式：

| 方式 | ASC 布局 | 对 legacy LC-only decoder |
|---|---|---|
| **Implicit** | `AOT=AAC_LC` + 标准 GASpecificConfig | 兼容：老 decoder 按 LC 解，忽略 fill_element 里的 SBR payload |
| **Explicit backward compatible** | `AOT=AAC_LC` + GASpecificConfig + `syncExtensionType=0x2B7` + `AOT_SBR` + `extSamplingFreqIdx` | 兼容 + 明确告知采样率 |
| **Explicit hierarchical** | `AOT=SBR` 直接写在 ASC 第一个字段 | 不兼容老 decoder |

FFmpeg 的 `libavcodec/mpeg4audio.c:108` 分支处理后两种显式路径；`libavcodec/aac/aacdec.c:1964` 的 `m4ac.sbr == -1 && status == OC_LOCKED` 就是 implicit 路径——第一个 fill_element 的 SBR payload 到了才"发现"有 SBR。implicit 的缺点是 decoder 必须能"事后升级"（把已经分配的输出 buffer/延迟线从 LC 的规格改成 HE 的规格），这就是 `AV_CODEC_CAP_CHANNEL_CONF` flag 存在的原因。

---

**Q25. 为什么 HE-AAC 在 SBR 生效时采样率是 "half rate" 模式？**

因为**核心 AAC 的 MDCT 做的是信号的低频一半**。编码端流程：

```
原 PCM (48 kHz, Nyquist 24 kHz)
   │
   ├─ QMF analysis 64 bands (每 band 750 Hz)
   │
   ├─ 下采样到 24 kHz (低 32 bands)  → AAC LC encode (核心)
   │                                    ↑ 这里 frame size = 1024 samples
   │                                    ↑ 但对应的"原始时间" = 2048 samples
   │
   └─ 高 32 bands → SBR envelope 提取 → SBR payload
```

所以 ASC 里通常这样写：`samplingFrequency=24000, extSamplingFrequency=48000`。Decoder 看到 `ext_sample_rate > sample_rate` 就知道要"双速率"——核心解出 1024 samples 的 24 kHz，SBR QMF synthesis 补上高频并升采样到 48 kHz，最终输出 2048 samples。`libavcodec/aacsbr_template.c:1688` 的 `downsampled = ext_sample_rate < sbr->sample_rate` 就是这个判断。

还有 "downsampled SBR mode"：有些低端 decoder 不做 QMF synthesis，只输出 24 kHz 的低频，此时 SBR payload 被丢弃，带宽腰斩——FFmpeg 也能通过 `m4ac.ext_sample_rate` 和 `sbr->sample_rate` 的比较走这条路径。

---

### 3F. Opus / Vorbis / FLAC

**Q26. Opus 的 hybrid mode：SILK + CELT，什么比特率用哪个？**

Opus 把整个比特率范围拆成三档，用 TOC byte 的 5 bit `config` 字段决定：

| config | 模式 | 比特率带宽 | 适合场景 |
|---|---|---|---|
| 0-11 | **SILK-only** | NB/MB/WB (8/12/16 kHz) | 低比特率语音 (6-40 kbps) |
| 12-15 | **Hybrid** (SILK+CELT) | SWB/FB (24/48 kHz) | 全带宽中低码率 (24-40 kbps) |
| 16-31 | **CELT-only** | NB/WB/SWB/FB | 音乐、低延迟、高码率 (32 kbps+) |

见 `libavcodec/opus/parse.c:253`：

```c
if (pkt->config < 12) {
    pkt->mode = OPUS_MODE_SILK;
    pkt->bandwidth = pkt->config >> 2;
} else if (pkt->config < 16) {
    pkt->mode = OPUS_MODE_HYBRID;
    pkt->bandwidth = OPUS_BANDWIDTH_SUPERWIDEBAND + (pkt->config >= 14);
} else {
    pkt->mode = OPUS_MODE_CELT;
    ...
}
```

SILK 是 Skype 开源的语音编码（LPC-based，擅长 voice），CELT 是 Xiph 的 MDCT-based 通用编码（擅长 music/low latency）。Hybrid 把 0-8 kHz 给 SILK、8-20 kHz 给 CELT，共用一个比特流，decoder 两条路径都跑然后合并。

---

**Q27. Opus 帧长可以是 2.5/5/10/20/40/60 ms，decoder 怎么知道每帧多长？**

也是 TOC byte 的 `config` 字段决定——每个 config 值里**同时编码**了模式 + 带宽 + 帧长。`libavcodec/opus/frame_duration_tab.c:21`：

```c
const uint16_t ff_opus_frame_duration[32] = {
    480, 960, 1920, 2880,   //  0-3  SILK NB      10/20/40/60 ms @48k
    480, 960, 1920, 2880,   //  4-7  SILK MB
    480, 960, 1920, 2880,   //  8-11 SILK WB
    480, 960,               // 12-13 Hybrid SWB   10/20 ms
    480, 960,               // 14-15 Hybrid FB    10/20 ms
    120, 240,  480,  960,   // 16-19 CELT NB      2.5/5/10/20 ms
    120, 240,  480,  960,   // 20-23 CELT WB
    120, 240,  480,  960,   // 24-27 CELT SWB
    120, 240,  480,  960,   // 28-31 CELT FB
};
```

数值单位是"@48 kHz 的样本数"，2.5 ms × 48 kHz = 120。注意 SILK 没有 2.5/5 ms（太短，LPC 没意义），CELT 没有 40/60 ms（太长，pre-echo 严重）。`pkt->frame_duration * pkt->frame_count` 总和必须 ≤ `OPUS_MAX_PACKET_DUR` (120 ms)，见 `libavcodec/opus/parse.c:250`。

---

**Q28. Opus 的 TOC (Table of Contents) 字节是什么？**

每个 Opus packet 的**第 1 字节**就是 TOC byte，解析见 `libavcodec/opus/parse.c:96`：

```c
i = *ptr++;
pkt->code   = (i     ) & 0x3;   // bit 0-1: code (0..3)
pkt->stereo = (i >> 2) & 0x1;   // bit 2  : mono/stereo
pkt->config = (i >> 3) & 0x1F;  // bit 3-7: 32 种配置（见 Q26/Q27）
```

`code` 字段决定这个 packet 里有几个 Opus frame：

| code | 含义 |
|---|---|
| 0 | 1 frame |
| 1 | 2 frames，等长 |
| 2 | 2 frames，不等长（第一个长度用 1-2 字节 xiph lacing 编码） |
| 3 | 1-48 frames，后跟 frame_count_byte（最高位 vbr，次高位 padding） |

对 VBR (`code=3` 且 vbr bit 设置)，每个 frame 的长度都要单独 xiph_lacing 编码。这让 Opus 能在一个 RTP 包里塞多个短 frame 摊薄头部开销，但同时 decoder 的 parser 必须能按 `xiph_lacing_16bit` 解析变长字段。

---

**Q29. Opus 的 `pre_skip` 字段（encoder delay）在哪里？**

在 OggOpus 的 **ID Header** 里（RFC 7845），FFmpeg 从 `avctx->extradata` 的偏移 10 读取：

`libavcodec/libopusdec.c:50,77`：

```c
#define OPUS_HEAD_SIZE 19
...
if (avc->extradata_size >= OPUS_HEAD_SIZE) {
    opus->pre_skip = AV_RL16(avc->extradata + 10);
    ...
}
...
avc->delay = opus->pre_skip;  // 汇报给 AVCodecContext
...
avc->internal->skip_samples = opus->pre_skip;  // decoder 输出时跳过前 N 样本
```

ID Header 布局：

```
0-7  : "OpusHead"
8    : version (=1)
9    : channel_count
10-11: pre_skip (LE 16, 单位: 48 kHz 样本)
12-15: input_sample_rate (LE 32, 参考值)
16-17: output_gain (LE 16, Q7.8 dB)
18   : channel_mapping_family
```

`pre_skip` 典型值 312 samples (6.5 ms)，对应 CELT 的 lookahead。flush 时 `libopusdec.c:213` 会重新设置 `skip_samples = pre_skip`，因为可以从任意 packet 重新开始解码。

---

**Q30. `libopusenc` 和 FFmpeg 内置的 `opusenc` 的关系？**

FFmpeg 里有**两个 Opus encoder**：

| encoder | 路径 | 来源 | 质量/成熟度 |
|---|---|---|---|
| `libopus` | `libavcodec/libopusenc.c` | 链接外部 libopus.so (Xiph 官方) | 参考实现，首选 |
| `opus` (原生) | `libavcodec/opus/enc.c` + `enc_psy.c` + `rc.c` | FFmpeg 内部重写 | 实验性，标 `AV_CODEC_CAP_EXPERIMENTAL` |

编译期由 `--enable-libopus` 控制是否编入 wrapper。两者都注册 `AV_CODEC_ID_OPUS`，`avcodec_find_encoder(AV_CODEC_ID_OPUS)` 按优先级返回 `libopus`（因为质量好）。内置 `opus` encoder 是从零重写的，早期是 GSoC 项目，只做 CELT 部分、SILK 还没写完——实际生产**不要用**，只做代码学习。Decoder 侧则相反：`libavcodec/opus/dec.c` 是主线路径（高质量、全功能），`libopusdec.c` 是可选 wrapper。

---

**Q31. Opus Ogg 封装里的 `granule_pos` 语义？**

Ogg 的 granule_pos 是一个 64 bit 单调递增计数器，每种 codec 有自己解释。对 OggOpus (`libavformat/oggparseopus.c`)：

- granule_pos **单位是 48 kHz 样本**，**与实际输出采样率无关**
- 表示 "这个 page 里最后一个样本的**解码后**时间戳"
- 第一个 audio page 的 granule_pos 减去 `pre_skip` 才是"第一个真正输出样本"的位置
- 最后一个 page 的 granule_pos 可以比实际少 N 个样本，表示末尾要裁 N 个 sample（给 Opus encoder 的 tail padding 切除用）

FFmpeg 靠这个做 Ogg 容器层的 seek：先二分找 granule_pos 接近目标的 page，定位到 page 边界，然后从那 page 开始解并丢弃 `skip_samples` 之前的数据。

---

**Q32. FLAC 帧结构：frame header + subframes + frame footer**

```
┌─ Frame Header
│  sync code    15 bit  = 0x7FFC (0b111111111111100 后跟 reserved)
│  blocking strategy 1 bit (fixed/variable)
│  block size  4 bit (索引，特殊值表示后面显式读 8/16 bit)
│  sample rate 4 bit (索引)
│  channel assignment 4 bit (左右/中差/左边 side/右边 side/独立通道)
│  sample size 3 bit
│  reserved    1 bit
│  sample/frame number: UTF-8 变长 (1-7 字节)
│  [block size explicit] 1-2 字节
│  [sample rate explicit] 1-2 字节
│  CRC-8       1 字节
│
├─ Subframe × channels
│  每个 subframe 有自己的 header (wasted bits + type) 和残差数据
│
├─ Frame Footer
│  padding to byte boundary
│  CRC-16     2 字节
└─
```

FLAC decoder 入口在 `libavcodec/flacdec.c`，`flac_parse.c` 负责 header 解析，subframe 解析见 `flacdec.c:524` 附近的 `decode_subframe`。"blocking strategy" 决定 sample number 是"帧号"还是"样本号"（VBR block size 时必须用样本号）。

---

**Q33. FLAC 的 LPC subframe 和 `order` 字段？**

LPC = Linear Predictive Coding。原理：假设当前样本可以用前 `order` 个样本的线性组合预测：

```
pred[n] = sum_{k=1..order} (coeff[k] * x[n-k]) >> qlevel
residual[n] = x[n] - pred[n]
```

残差熵比原信号低很多，Rice code 高效压缩。decoder 路径：

`libavcodec/flacdec.c:437` `decode_subframe_lpc`：
1. 读 `qlp_coeff_precision`（4 bit，+1 = 实际精度 1-16 bit）
2. 读 `qlp_shift`（5 bit，signed）= qlevel
3. 读 `order` 个量化系数（每个 precision bits）
4. 用 `s->dsp.lpc16` 或 `lpc32` 把系数 + 残差合成回 PCM

order 范围 1-32（`type >= 32` 在 subframe type 编码里，type=32+order-1，见 `flacdec.c:585` 的 `(type & ~0x20)+1`）。高 order 能压得更好但 encoder/decoder 都更慢。

---

**Q34. FLAC 的 CONSTANT / VERBATIM / FIXED / LPC 四种 subframe 类型**

`libavcodec/flacdec.c:553`：

```c
if (type == 0) {
    // CONSTANT: 整个 subframe 一个值
    int32_t tmp = get_sbits_long(&s->gb, bps);
    for (i = 0; i < s->blocksize; i++) decoded[i] = tmp;
} else if (type == 1) {
    // VERBATIM: 原始 PCM，不压缩（给无法预测的信号兜底）
    for (i = 0; i < s->blocksize; i++)
        decoded[i] = get_sbits_long(&s->gb, bps);
} else if ((type >= 8) && (type <= 12)) {
    // FIXED: 预定义固定系数 LPC，order = type & 0x7 (0..4)
    decode_subframe_fixed(...);
} else if (type >= 32) {
    // LPC: 自定义系数 LPC，order = (type & 0x1F) + 1 (1..32)
    decode_subframe_lpc(...);
}
```

| 类型 | type bits | order | 用途 |
|---|---|---|---|
| CONSTANT | 000000 | - | DC 信号（静音） |
| VERBATIM | 000001 | - | 兜底，encoder 放弃预测 |
| FIXED | 001000-001100 | 0-4 | 固定预测器（Shorten 兼容），encoder 快 |
| LPC | 1xxxxx | 1-32 | 训练的 LPC，压缩率最高 |

Encoder 对每个 subframe 试所有类型，选 bit 数最少的。`libavcodec/flacenc.c` 里的 `encode_residual_ch` 实现这个比较。

---

**Q35. FLAC 的 Rice coding 残差编码**

Rice code = Golomb code with power-of-2 divisor，是 LPC residual 的高效熵编码：

1. 把 residual 用 zigzag 映射到非负整数 `u = (r < 0) ? (-2r - 1) : (2r)`
2. 用参数 `k` 把 `u` 拆成商 `q = u >> k` 和余 `r' = u & ((1<<k)-1)`
3. `q` 用 unary 编码（`q` 个 1 后跟一个 0），`r'` 直接 `k` bit 输出

关键：`k` 参数可以**每个 partition 单独选**。FLAC 把 subframe 的 `blocksize` 个残差分成 `2^partition_order` 份，每份选最优 `k`（4 bit 编码 METHOD_PARTITIONED_RICE，5 bit 编码 METHOD_PARTITIONED_RICE2 支持更大 k）。见 `libavcodec/flacdec.c` 的 `decode_residuals` 和 `golomb.h` 的 `get_sr_golomb_flac`。

压缩率：residual 近似 Laplacian 分布时，Rice code 是理论最优熵编码的 0.04 bit/sample 以内。

---

**Q36. FLAC 的 `STREAMINFO` metadata block 关键字段**

STREAMINFO 是 FLAC 文件第一个必选 metadata block，34 字节定长，FFmpeg 里直接作为 `extradata`。解析见 `libavcodec/flac.c:187` `ff_flac_parse_streaminfo`：

```c
min_blocksize    16 bit (skip)
max_blocksize    16 bit  → s->max_blocksize  (决定 decoder 分配缓冲大小)
min_framesize    24 bit (skip)
max_framesize    24 bit  → s->max_framesize
samplerate       20 bit  → avctx->sample_rate (0 ~ 655350 Hz)
channels          3 bit  → +1 后实际 1-8
bps               5 bit  → +1 后 4-32 (每样本位数)
total_samples    36 bit  → s->samples (文件总样本数，0 = 未知)
md5 signature   128 bit  (未压缩 PCM 的 MD5，skip)
```

关键点：
- `samplerate` 是 20 bit，所以 FLAC 最高支持 **1048575 Hz**（远大于任何实际应用）
- `total_samples` 是 36 bit，max ≈ 6.8 × 10^10 samples @ 44.1 kHz ≈ 18 天
- `md5` 字段 encoder 可以不填（全 0），decoder 不必校验
- `max_blocksize` 是 decoder 预分配 `decoded_buffer` 的关键依据

---

**Q37. Vorbis 的 3 个 setup headers 和 extradata 存储格式**

Vorbis I 规范要求流开始有 3 个 header packet：

1. **Identification header** (type 1): 采样率、声道数、比特率上下限、blocksize_0/1
2. **Comment header** (type 3): Vorbis comment (类似 FLAC 的 Vorbis tag，KEY=VALUE)
3. **Setup header** (type 5): codebooks, floors, residues, mappings, modes—解码器核心配置

FFmpeg 把三个 header 打包成 `extradata`，格式有两种：

- **xiph 模式** (Matroska/WebM 用): 
  ```
  packet_count - 1 (1 字节, = 2)
  len_header1 (xiph lacing: 255,255,...,余数)
  len_header2 (xiph lacing)
  header1 bytes
  header2 bytes
  header3 bytes  (长度隐含: 总长 - 前面)
  ```
- **ffmpeg 模式**: 直接拼接，用 `ff_vorbis_extradata_create` 生成

`libavcodec/vorbisdec.c` 的 `vorbis_decode_init` 解 extradata，然后逐个 header 调用 `vorbis_parse_id_hdr` / `vorbis_parse_setup_hdr`。Setup header 最大——codebook 表可以几十 KB。

---

**Q38. Vorbis 的 granule_pos 和 PCM sample 的关系**

对 Ogg Vorbis（`libavformat/oggparsevorbis.c`）：

- granule_pos **单位是 PCM 样本数**（不是 48 kHz，而是流自己的 sample rate）
- 表示 page 内最后一个 complete packet 的**解码后末尾样本**的 PCM index
- Vorbis 的特殊规则：**packet 跨 page 时**，granule_pos 只反映 page 里"完成"的那些 packet
- 第一个 audio page 的 granule_pos 可能 > 该 page 数据解出的样本数——差值就是 "initial padding"（类似 AAC encoder delay，来自 MDCT overlap-add 的冷启动）
- 最后一个 page 的 granule_pos 可能 < 累积样本数——差值是 "tail padding"，decoder 要裁掉末尾

这和 Opus 不同（Opus 总用 48 kHz 单位），比 Opus 更早定义、更"自然"但更难跨采样率对齐。

---

**Q39. Vorbis 的 floor / residue 分离**

Vorbis 的 MDCT 系数解码分两条路径：

- **Floor**: 频谱**包络**（慢变化的幅度曲线），有两种类型:
  - `floor 0`: LSP/LSF (cosines of line spectral pairs)——几乎没人用
  - `floor 1`: 分段线性曲线（x, y 对），控制点索引从 codebook 解出来
- **Residue**: "细节" = 真实 MDCT 系数除以 floor。3 种类型：
  - `residue 0/1/2`: 不同的 codebook 排列策略（interleaved vs partitioned）

解码流程：

```
packet → mode → mapping → 
  ├─ decode floor (每通道) → 得到频谱包络
  ├─ decode residue → 得到原始频谱系数 (已经是"包络乘以细节"的形态)
  └─ inverse coupling (通道间耦合)
    ↓
  inverse MDCT → overlap-add → PCM
```

`libavcodec/vorbisdec.c` 的 `vorbis_floor` / `vorbis_residue` 是主要的数据结构（见 `vorbisdec.c:69-110`）。这种 "envelope + residual" 的分离让 Vorbis 在低码率下也能保持频谱结构不崩——floor 即使被粗量化，耳朵也听不出"掉频段"。

---

**Q40. 为什么 Ogg 容器不能精确 seek 到任意毫秒（只能 seek 到 page 边界）？**

Ogg 的最小 **寻址单元是 Page**（典型 4-8 KB，包含多个 packet）：

1. **Page 内部没有索引**——要拿到 page 里某个 packet 必须从 page 头线性扫描
2. Ogg 没有全局 seek 索引表（不像 MP4 的 `stco/ctts`）
3. seek 策略：**二分搜索 page**，读 granule_pos，比目标大/小调整——`libavformat/oggdec.c:773` 的 "linear granulepos seek from end"
4. 找到"第一个 granule_pos ≥ 目标"的 page 后，从该 page 的前一个 page 开始解（因为 packet 可能跨 page），然后在 decoder 输出流里丢弃多余的 sample
5. codec 层面的精度由 `skip_samples` side data 补齐——这部分对用户是透明的

结果：
- **page 粒度**：文件只能在 page 边界做"粗 seek"（可能误差几百 ms）
- **sample 粒度**：通过"粗 seek + 多解几个 page + 丢弃前缀"达到 sample 精确，但代价是额外解码
- 对比 MP4/MKV：有 sample-level 的索引表，seek O(log n) 直接到 sample；Ogg 是 O(log n) × "扫整个 page"，常数大得多

这也是为什么 web 上的 Opus/Vorbis 通常封在 WebM/Matroska 而不是 Ogg——Matroska 的 Cueing Data 有 sample 级索引。

---

### 3G. libswresample 深度（Q1–Q20）

### Q1. `swr_alloc_set_opts2` 和老 API `swr_alloc_set_opts` 的参数差异？

| 维度 | 旧 `swr_alloc_set_opts`（已删） | 新 `swr_alloc_set_opts2`（`libswresample/swresample.h:259`） |
|---|---|---|
| 通道参数 | `int64_t out_ch_layout` + `int64_t in_ch_layout`（位掩码） | `const AVChannelLayout *out_ch_layout` + `const AVChannelLayout *in_ch_layout` |
| 返回约定 | 返回 `struct SwrContext *` | 返回 `int`（错误码），出参 `SwrContext **ps` |
| 失败处理 | 失败返回 NULL，资源由调用者管理 | 失败时 `*ps` 被置 NULL 并释放，语义更干净 |
| 通道表达力 | 只能表达 64 位 mask 之内的布局，不支持 Ambisonic、16+ 通道 | `AVChannelLayout` 支持 `NATIVE / CUSTOM / AMBISONIC / UNSPEC` 四种 order |

旧 API 在 FFmpeg 5.x 被标记 deprecated、6.x 彻底移除。现在 `swr_alloc_set_opts2` 是唯一入口，旧的 `swr_alloc_set_opts` 名字已经不存在。

本质原因：5.1 的通道布局重构把 "`channels + channel_layout` 两个不一致的字段" 合并成一个自描述结构。老 API 的签名携带 `int64_t`，无法前向兼容，只能另开一个 "2"。

---

### Q2. `swr_init` 失败的典型原因有哪些？

看 `libswresample/swresample.c:swr_init`，失败路径大概这几类：

| 错误码 | 触发条件 | 源码位置 |
|---|---|---|
| `AVERROR(EINVAL)` | `swri_check_chlayout` 失败：布局非法、`nb_channels == 0`、CUSTOM 无 map | `swresample.c` 前半段 |
| `AVERROR(EINVAL)` | `used_ch_layout.nb_channels != in_ch_layout.nb_channels` 且 in 非 UNSPEC | `swresample.c:321-325` |
| `AVERROR(EINVAL)` | 输入或输出是 UNSPEC，无法决定 rematrix 矩阵 | `swresample.c:327-333` |
| `AVERROR(EINVAL)` | 重采样 filter 参数非法（`filter_size` / `phase_shift` 太大溢出） | `resample.c` |
| `AVERROR(ENOMEM)` | `av_calloc` 分配失败（`AudioData::data`、polyphase 滤波器系数表） | `swresample.c:427 / resample.c:41` |
| `AVERROR(ENOMEM)` | `swri_audio_convert_alloc` 分配 `in_convert / out_convert` 失败 | `swresample.c:357` |

面试加分：**UNSPEC 的容错是工程决定，不是 spec**——swresample 遇到 UNSPEC 会按 `nb_channels` 填一个 NATIVE 默认布局，但如果 in 和 out 有一个仍然 UNSPEC 且 `used_ch_layout.nb_channels != out.ch_count` 就直接拒。

---

### Q3. `SwrContext` 的内部 AudioData 5 阶段分别是什么格式？

见 `libswresample/swresample_internal.h:146-153`：

```c
AudioData in;       // 用户输入端指针 wrapper
AudioData postin;   // 输入 → 内部格式 后的临时区
AudioData midbuf;   // rematrix 之间的中间态
AudioData preout;   // 送进 out_convert 之前
AudioData out;      // 用户输出端指针 wrapper
```

各自的 `fmt` 在 `swr_init` 里设置（`swresample.c:378-384`）：

| 阶段 | 样本格式 | 通道数 | 说明 |
|---|---|---|---|
| `in` | `in_sample_fmt`（如 `FLTP`） | `in.ch_count` | 用户输入 |
| `postin` | `int_sample_fmt`（fltp/s16p/s32p/dblp） | `used_ch_layout.nb_channels` | 输入完成转换到 "工作格式" |
| `midbuf` | `int_sample_fmt` | `resample_first ? used_ch : out.ch_count` | 重采样/混音之间的 buffer |
| `preout` | `int_sample_fmt` | `out.ch_count` | 最后一步进入 out_convert 前 |
| `out` | `out_sample_fmt`（如 `S16`） | `out.ch_count` | 用户输出 |

`int_sample_fmt`（internal sample format）通常是 `FLTP`，`s16`/`s32` 目标时会退化到 `S16P`/`S32P`。整个流水线：**in → in_convert → postin → rematrix₁ → midbuf → resample → midbuf/preout → rematrix₂ → preout → out_convert → out**。

---

### Q4. `swresample.c:337` 的 `resample_first` 启发式判据？

```c
s->resample_first = RSC * s->out.ch_count / s->used_ch_layout.nb_channels
                    - RSC < s->out_sample_rate / (float)s->in_sample_rate - 1.0;
```

`RSC` 是"重采样相对成本常数"。两边意义：

- 左边 `RSC * ch_out/ch_in - RSC = RSC * (ch_out - ch_in) / ch_in`
- 右边 `rate_out/rate_in - 1 = (rate_out - rate_in) / rate_in`

等价判据：**"相对通道数扩张 < 相对采样率扩张"**。
- 如果输出采样率远大于输入（大幅上采），应该**先 rematrix 再 resample**（`resample_first = 0`），因为 resample 后样本数变多，rematrix 成本会乘倍。
- 如果通道数大幅增加（上混），应该**先 resample 再 rematrix**（`resample_first = 1`），因为上混后通道数多，重采样成本乘倍。

核心思想：**把成本乘法因子放在流水线后半**，让慢的那一步只面对少的数据。

---

### Q5. 为什么 swresample 有"工作格式"（通常 s16p/s32p/fltp/dblp），不直接在输入输出之间转？

三层理由：

1. **数学正确性**：rematrix 是浮点矩阵乘法（`s->matrix[out][in]`）。如果直接对 `u8` 或 `s16` interleave 格式算混音，每次乘加都需要 unpack/pack + saturate，代码复杂且慢。
2. **planar 假设**：重采样器 `resample_common_TMPL` 每个通道独立处理，**要求 planar**。packed 输入必须先"拆 plane"。
3. **共享 SIMD 内核**：`libswresample/resample_template.c` 只对 `s16p/s32p/fltp/dblp` 四种格式实例化。输入是 `u8`？它会先被转 `s16p`，让下游所有代码只面对 4 种情况。

副作用：`int_sample_fmt` 总是 planar 变体，`AV_SAMPLE_FMT_S16P / S32P / FLTP / DBLP` 之一。`dither_init`（`dither.c:80`）还会据此选择 dither 算法。

`swresample.c:346` 的快速路径：只有**不需要 resample、不需要 rematrix、不需要 dither、无 channel_map** 时才会走 `full_convert`（一次性 `in_sample_fmt → out_sample_fmt`），其他一律走完整 5 级流水线。

---

### Q6. `swri_realloc_audio` 什么时候被调？AudioData buffer 扩容？

定义在 `libswresample/swresample.c:408-438`：

```c
int swri_realloc_audio(AudioData *a, int count){
    if(a->count >= count) return 0;   // 现有足够，直接返回
    count *= 2;                       // 2× 扩容，避免频繁 realloc
    countb = FFALIGN(count*a->bps, ALIGN);
    a->data = av_calloc(countb, a->ch_count);
    // 重新切分 ch[i] 指针，planar 一列一段，packed 一整段
    ...
    av_freep(&old.data);
}
```

调用点（`libswresample/swresample.c:562 / 606 / 610 / 614 / 617 / 675 / 679 / 744 / 814 / 865`，以及 `resample.c:443 / 465`）：

| 调用点 | 时机 |
|---|---|
| `swr_convert_internal` 入口 | 预扩 `postin` / `midbuf` / `preout` / `in_buffer`（按需） |
| `swr_convert` 写入 `in_buffer` | 输入样本数 > 当前 buffer 容量 |
| dither 路径 | 分配 `dither.temp / dither.noise` |
| `swr_drop_output` | `drop_temp` 扩容 |
| `swr_inject_silence` | `silence` 扩容 |
| `resample_flush` (`resample.c:443`) | flush 时需要 reflection padding，buffer 额外扩 filter_length/2 |
| `invert_initial_buffer` (`resample.c:465`) | 冷启动 buffer：`filter_length*2+1` |

关键是 **2× 扩容策略 + `av_calloc`**：避免反复分配，同时保证扩容时新区域清零（dither noise 需要）。

---

### Q7. `swr_convert` 输出样本数计算为什么要加 `swr_get_delay(s, in_rate)`？

重采样器内部维护一个输入缓冲（`in_buffer`），因为 polyphase FIR 需要"中心样本前后各 filter_length/2 个样本"才能产出一个输出。所以 swresample 不会"来一个出一个"，总是滞后若干样本。

`swr_get_delay(s, base)` 返回**当前内部延迟缓冲对应的样本数**（`resample.c:408-416`）：

```c
int64_t num = s->in_buffer_count - (c->filter_length-1)/2;
num *= c->phase_count;
num -= c->index;
num *= c->src_incr;
num -= c->frac;
return av_rescale(num, base, s->in_sample_rate * (int64_t)c->src_incr * c->phase_count);
```

正确分配输出 buffer 的姿势：

```c
int64_t delay = swr_get_delay(swr, in_rate);
int out_samples = av_rescale_rnd(delay + in_samples, out_rate, in_rate, AV_ROUND_UP);
av_samples_alloc(&out_buf, NULL, nb_ch, out_samples, out_fmt, 0);
```

**为什么不能省掉 delay？** 如果你上一轮调用 `swr_convert` 只送进去 1000 样本，但因为 filter 还没填满没有任何输出，这 1000 样本全部滞留在内部。下一轮你再送 1000 样本时，理论上输出应该按 2000 样本算，否则 buffer 会不够。`delay` 就是补偿这部分"过去塞进去但还没吐出来"的数据。

---

### Q8. polyphase FIR filter 的 `filter_length` / `phase_shift` / `phase_count` 关系？

见 `swresample_internal.h:125-127`：

```c
int filter_size;    // 每个 FIR 的 tap 数（相对 cutoff 归一化）
int phase_shift;    // log2 of phase_count
```

以及 `ResampleContext`（`resample.h`）里：
- `filter_length`：实际 tap 数（`= filter_size` 或经过 rational 修正）
- `phase_count = 1 << phase_shift`（除非 `exact_rational = 1`）

物理意义：
- **phase_count**：把理想 sinc 滤波器切成 `phase_count` 个 polyphase 分支，每个分支是原 sinc 按 `1/phase_count` 偏移采样后的 FIR。
- **filter_length**：每个分支的 tap 数。总系数数 = `phase_count * filter_length`。
- **phase_shift = 10**（默认）→ `phase_count = 1024`：把相位量化到 1024 档。1024 档之间再做 linear_interp（`linear_interp = 1`）能进一步平滑，但默认不开。

44.1k → 48k 的做法：`phase_count × in_rate = 1024 × 44100`, 每次输出要走 `1024 × 44100 / 48000 ≈ 940.8` 个 phase 步，小数部分累计到 `frac`。实际代码是有理数版本：`phase_count * out_rate / in_rate` 作为 `dst_incr_div`，余数作为 `dst_incr_mod`，类似 Bresenham 画线。

---

### Q9. Kaiser 窗的 `beta` 对频率响应的影响？

Kaiser 窗：`w[n] = I₀(β · √(1 - (2n/N - 1)²)) / I₀(β)`。`I₀` 是零阶修正贝塞尔函数（`av_bessel_i0`，`mathematics.c`）。

面试简答：**β 越大，主瓣越宽、旁瓣越低**。工程折中：

| β | 旁瓣衰减 | 主瓣宽度（过渡带） | 应用 |
|---|---|---|---|
| 0（矩形窗） | -13 dB | 最窄 | 不用 |
| 4 | ~-30 dB | 窄 | 快而糙 |
| 6 | ~-60 dB | 中 | 通用 |
| 9.42（swresample 默认） | ~-90 dB | 较宽 | 高保真 |
| 12 | ~-114 dB | 宽 | Studio quality |

swresample 在 `resample.c:86-89` 里的实现：
```c
case SWR_FILTER_TYPE_KAISER:
    w = 2.0*x / (factor*tap_count*M_PI);
    y *= av_bessel_i0(kaiser_beta*sqrt(FFMAX(1-w*w, 0)));
```

和 sinc（`y = sin(x)/x`）相乘得到最终 tap 系数。`beta` 越大意味着"更锐利的低通"，但需要更长的 `filter_length` 才能真正达到那个旁瓣指标——**纯调大 beta 不调 filter_length 是假高精度**。

---

### Q10. 重采样器初始化 `build_filter` 做了什么？

`libswresample/resample.c:41-131`：

1. 分配 `tab[tap_count+1]` 和 `sin_lut[ph_nb]`（`ph_nb = phase_count/2 + 1`，利用偶对称只算一半相位）。
2. 外层 `for ph = 0..ph_nb`：遍历所有相位。
3. 对每个相位填 `tab[0..tap_count-1]` = sinc × 窗（三选一：CUBIC / BLACKMAN_NUTTALL / KAISER）。
4. 按 `filter_type` 应用窗函数。
5. **归一化**：`norm = sum(tab[i])`，再按 `c->format` 转成目标精度：
   - `S16P` → `av_clip_int16(lrintf(tab[i] * scale / norm))`
   - `S32P` → `av_clipl_int32(llrint(tab[i] * scale / norm))`
   - `FLTP` → `tab[i] * scale / norm`
   - `DBLP` → `tab[i] * scale / norm`
6. **对称镜像**：`phase_count % 2 == 0` 时用 `filter[ph_nb .. phase_count]` 就是 `filter[0 .. ph_nb]` 的反序拷贝，省一半计算。

`factor = min(1.0, out_rate/in_rate)`：**下采样时用 factor < 1 缩窄 cutoff**，防止 alias；上采样 factor = 1。

性能点：整个 filterbank 是**预计算**的，运行时只做 FIR 乘加，不再做 sinc/窗计算。

---

### Q11. `resample.c:408-416` 的 flush 路径怎么吐出内部延迟？

这里是 `get_delay` 而不是 flush 主路径。真正的 flush 在 `resample_flush`（`resample.c:437-454`）和 `swr_convert(NULL, ...)` 协作：

1. 用户调 `swr_convert(s, out, out_count, NULL, 0)`。
2. `swresample.c:763-771`：`in_arg == NULL` → 调 `s->resampler->flush(s)`，设置 `s->flushed = 1`。
3. `resample_flush` 在 `in_buffer` 尾部追加 `reflection = (FFMIN(in_buffer_count, filter_length) + 1) / 2` 个"镜像"样本。
4. 然后走正常 `multiple_resample` 路径，让 FIR 把那些延迟样本吐出来。

由于 reflection 样本是"对最后若干真实样本做时间反转"，FIR 在处理最末几个输出时不会看到硬截断（会变成一个软的对称结束），**大幅减少末端 click 噪声**。

`get_delay` 只是查询函数，返回"还有多少输入被缓冲"，乘以时间基后可以换算成秒或样本数。

---

### Q12. flush 时的"reflection padding"为什么这么做？

代码 `resample.c:437-454`：

```c
int reflection = (FFMIN(s->in_buffer_count, c->filter_length) + 1) / 2;
swri_realloc_audio(a, s->in_buffer_index + s->in_buffer_count + reflection);
for(i=0; i<a->ch_count; i++){
    for(j=0; j<reflection; j++){
        memcpy(a->ch[i] + (s->in_buffer_index+s->in_buffer_count+j)*a->bps,
               a->ch[i] + (s->in_buffer_index+s->in_buffer_count-j-1)*a->bps, a->bps);
    }
}
s->in_buffer_count += reflection;
```

三种可能的 flush 填充方式及为什么选 reflection：

| 方式 | 问题 |
|---|---|
| **零填充** | FIR 把最后几个样本和 0 做卷积，等价于一个生硬的 "降到 0" 的台阶，在末端制造 click + ringing |
| **静音重复** | 和零填充等价 |
| **重复最后一个样本** | 相当于在末端加 DC 分量，引入低频 artifacts |
| **Reflection**（当前做法） | 信号在末端"对称反转"延续，导数（斜率）平滑，FIR 输出不会有不连续，末端 click 最小 |

数学上，reflection 等价于在末端做"偶延拓"，相当于让信号在边界处变成偶函数。对**带限信号** + 对称 FIR filter，偶延拓的卷积结果最接近"如果后续数据一直存在"的理想输出。

长度选 `filter_length/2`：FIR 只会用到"中心±filter_length/2"的样本，反射这么多就够覆盖整个 filter 窗口。

---

### Q13. `swri_rematrix` 的矩阵构造（`libswresample/rematrix.c`）？

主干在 `build_matrix`（`rematrix.c:134-410`）和 `auto_matrix`（`438-455`）：

1. **对角复制**：输入和输出都有的通道（如 FL → FL）直接 `matrix[i][i] = 1.0`。
2. **逐"未覆盖"通道处理**：`unaccounted = (in_mask) & ~(out_mask)`，对每个 bit 分别添加混音规则。
3. **规则举例**：
   - `FRONT_CENTER` 未被覆盖，但输出有 stereo → `matrix[FL][FC] += clev`, `matrix[FR][FC] += clev`
   - `BACK_LEFT/RIGHT` 未被覆盖，输出有 front → `matrix[FL][BL] += slev`, `matrix[FR][BR] += slev`
   - `LFE` 不在输出 → **默认丢弃**（或按 `lfe_mix_level` 加到 FL/FR）
4. **归一化**：`maxcoef = max(matrix)`；如果 `maxcoef * maxval > INT16_MAX` 就整体除以 `maxcoef` 防溢出。
5. **`swri_rematrix_init`（`rematrix.c:457-563`）**：把 `double matrix[][]` 烘焙到内部格式：
   - `FLTP` → `native_matrix` = float 数组
   - `DBLP` → `native_matrix` = double 数组
   - `S16P/S32P` → `native_matrix` = int 数组（按 32768 缩放 + 余数误差累加）
6. **生成 `mix_any_f / mix_1_1_f / mix_2_1_f`**：根据每个输出通道引用的输入通道数选择调度函数。`matrix_ch[out_i][0]` 是该输出通道非零系数的个数，`matrix_ch[out_i][1..]` 是那些输入通道的 index。

运行时（`swri_rematrix` 函数，`rematrix.c:570-650`）：
- 0 个输入 → memset
- 1 个输入 → `mix_1_1_f`
- 2 个输入 → `mix_2_1_f`
- ≥3 个 → 通用循环或 `mix_any_f`

---

### Q14. 5.1 → 2.0 下混的 ITU-R BS.775 系数？

ITU-R BS.775 规定的标准下混公式（也就是 swresample 默认做的）：

```
L = FL + clev·FC + slev·BL
R = FR + clev·FC + slev·BR
```

其中：
- `clev = 1/√2 ≈ 0.707`（`M_SQRT1_2`，中置衰减 -3 dB）
- `slev = 1/√2 ≈ 0.707`（环绕衰减 -3 dB）
- `LFE` 乘以 `lfe_mix_level`，默认为 0（丢弃）——见 `SwrContext::lfe_mix_level`（`swresample_internal.h:110`）

代码对应：
- `rematrix.c:154-165` 处理 `FC → (FL, FR)`：`matrix[FL][FC] += M_SQRT1_2; matrix[FR][FC] += M_SQRT1_2;`
- `rematrix.c:214-228` 处理 `BL/BR → (FL, FR)`：默认 `matrix[FL][BL] += slev`（surround_mix_level，默认也是 `M_SQRT1_2`）。

SwrContext 默认：`clev = s->clev = M_SQRT1_2`（`options.c`），`slev = M_SQRT1_2`，`lfe_mix_level = 0`。

---

### Q15. Dolby Surround / DPL2 下混的特殊相位反转（`rematrix.c:215-224`）？

普通立体声下混后，原 5.1 的后声道在回放时从立体声变回 surround 就丢了。**Dolby Surround Encoding** 通过给 BL/BR 做 180° 相位反转 + 混入 FL/FR，让后续 Pro Logic 解码器能从相位差把它们再分离出来。

源码（`rematrix.c:215-224`）：

```c
} else if (matrix_encoding == AV_MATRIX_ENCODING_DOLBY) {
    matrix[FRONT_LEFT ][BACK_LEFT ] -= surround_mix_level * M_SQRT1_2;
    matrix[FRONT_LEFT ][BACK_RIGHT] -= surround_mix_level * M_SQRT1_2;
    matrix[FRONT_RIGHT][BACK_LEFT ] += surround_mix_level * M_SQRT1_2;
    matrix[FRONT_RIGHT][BACK_RIGHT] += surround_mix_level * M_SQRT1_2;
} else if (matrix_encoding == AV_MATRIX_ENCODING_DPLII) {
    matrix[FRONT_LEFT ][BACK_LEFT ] -= surround_mix_level * SQRT3_2;
    matrix[FRONT_LEFT ][BACK_RIGHT] -= surround_mix_level * M_SQRT1_2;
    matrix[FRONT_RIGHT][BACK_LEFT ] += surround_mix_level * M_SQRT1_2;
    matrix[FRONT_RIGHT][BACK_RIGHT] += surround_mix_level * SQRT3_2;
}
```

- **Dolby Surround**：对 BL 和 BR **取负号混到 FL**，正号混到 FR。解码器根据"FL 和 FR 相位差 180°"的成分判定 surround。
- **DPL2**：系数改成 `SQRT3_2 = √1.5 ≈ 1.2247` 和 `M_SQRT1_2 ≈ 0.707` 的非对称组合。这个非对称是 DPL2 比原始 Dolby Surround 多出的"更精确的方向感"（Steered），使得前后环绕分离度更高。

`SQRT3_2` 常量定义在 `swresample_internal.h:30`：`#define SQRT3_2 1.22474487139158904909`。

**工程意义**：这是 swresample 对"做下混但保留后续再上混能力"的保守实现。只有调用方显式设置 `matrix_encoding = AV_MATRIX_ENCODING_DOLBY/DPLII` 才会启用，默认走 Q14 的无相位反转公式。

---

### Q16. `swri_rematrix_init` 里的量化：s16 为何是 15 位？

看 `rematrix.c:469-495`：

```c
if (s->midbuf.fmt == AV_SAMPLE_FMT_S16P) {
    s->native_matrix = av_calloc(nb_in * nb_out, sizeof(int));
    for (i = 0; i < nb_out; i++) {
        double rem = 0;
        int sum = 0;
        for (j = 0; j < nb_in; j++) {
            double target = s->matrix[i][j] * 32768 + rem;
            ((int*)s->native_matrix)[i * nb_in + j] = lrintf(target);
            rem += target - ((int*)s->native_matrix)[i * nb_in + j];
            sum += FFABS(((int*)s->native_matrix)[i * nb_in + j]);
        }
        maxsum = FFMAX(maxsum, sum);
    }
    s->native_one.i = 32768;
    ...
}
```

- **`32768 = 2^15`** —— 量化到 **Q15 定点数**（小数点在第 15 位）
- `sample_s16 * coeff_q15 >> 15 = 输出_s16`，乘法结果是 `int32`，刚好在 64 位寄存器里不溢出（`s16 * s16 = s32`，再累加 nb_in 项：需要 `s32 + log2(nb_in)` 位头部空间）
- `rem` 是 **误差反馈累加器**：把当前通道的 round 余数带到下一个通道的系数里，保证每行系数之和的量化误差 ≤ 1 LSB，否则简单 `lrint` 每行都有独立 bias，和可能偏离 1.0 导致整体音量漂移
- `maxsum` 用来判断是否需要 clip：如果某行系数绝对值之和 ≤ 32768，输出不可能溢出 s16，可以用非 clip 的 `copy_s16 / sum2_s16`；否则必须用 `copy_clip_s16 / sum2_clip_s16`

**注意：s32 也用 32768 缩放**（`rematrix.c:519-531`），不是 30 位。这是因为 s32 样本本身可以非常大，乘以 15 位系数后用 `int64` 累加 + 右移 15 位即可得 s32 结果。真要 30 位精度，中间结果需要 `int64 + 30` 位头部空间，移位成本反而增加。面试题目里给的"30 位 for s32"**是个陷阱**——源码实际是 Q15。

---

### Q17. `swr_set_compensation` 和 PLL 式软同步？

定义在 `swresample.c:909-940`。用途：**音频和视频时钟的轻微漂移软校正**。

```c
int swr_set_compensation(SwrContext *s, int sample_delta, int compensation_distance);
```

- `sample_delta`：在 `compensation_distance` 个输出样本内，多吐或少吐 `sample_delta` 个样本。
- `compensation_distance == 0`：取消补偿。

内部做法：**临时修改 `dst_incr`**（polyphase Bresenham 的步长），让重采样比率从 `out_rate/in_rate` 微偏到 `(out_rate ± delta)/in_rate`，持续 `compensation_distance` 个样本后恢复。见 `resample.c:396-403`：

```c
if (c->compensation_distance) {
    c->compensation_distance -= dst_size;
    if (!c->compensation_distance) {
        c->dst_incr     = c->ideal_dst_incr;
        c->dst_incr_div = c->dst_incr / c->src_incr;
        c->dst_incr_mod = c->dst_incr % c->src_incr;
    }
}
```

**PLL 式软同步**：`swresample.c:937-956` 的 `swr_next_pts` 自动做这件事——它计算 `fdelta = (outpts_expected - outpts_actual)`，如果超过 `min_compensation` 就调用 `swr_set_compensation(s, comp, duration)`。`duration = s->out_sample_rate * s->soft_compensation_duration`，把"每秒需要微调多少样本"平滑地摊到一秒内，**听感上感觉不到跳变**。如果超过 `min_hard_compensation` 则直接 `swr_inject_silence` 或 `swr_drop_output`，硬跳。

面试加分：这是一种 "第一类 PLL"（proportional only）反馈——没有积分项，单纯看当前 phase error 做比例修正。足够对付"视频 1% pitch drift"这种慢漂移，对不上音视频完全不同步（初相大错）要靠调用方自己调 `firstpts`。

---

### Q18. Dither：`S16` 目标格式下的 triangular / rectangular / high pass dither？

定义在 `libswresample/dither.c`，枚举见 `swresample.h`：
- `SWR_DITHER_NONE`
- `SWR_DITHER_RECTANGULAR`
- `SWR_DITHER_TRIANGULAR`
- `SWR_DITHER_TRIANGULAR_HIGHPASS`
- `SWR_DITHER_NS*`（noise shaping 若干变种）

`swri_dither_init`（`dither.c:80`）：**当输入是浮点格式且输出 bit 数更低**（如 `FLTP → S16`），自动补一个 dither（默认 `SWR_DITHER_TRIANGULAR_HIGHPASS`，见 `dither.c:129`）。

三种基本 dither：

| 类型 | 噪声分布 | 频谱特性 | 公式（`dither.c:41` 附近） |
|---|---|---|---|
| **Rectangular** | 均匀分布 `U(-0.5, 0.5)` | 白 | `v = seed/UINT_MAX - 0.5` |
| **Triangular**（TPDF） | 两个独立 U 求和，近似三角分布 `[-1, 1]` | 白，方差更大但无 pattern | `v = (u1 + u2) - 1` |
| **Triangular Highpass** | TPDF + 一阶差分 | **把噪声能量推到高频**（人耳不敏感） | `v = (cur_u - prev_u)` |

**为什么需要 dither**：没有 dither 的 `floor(x * 32767)` 会在低电平（`-60 dB` 以下）出现**量化死区**——正弦波变成台阶。Dither 把死区变成白噪声（或三角形噪声），让量化误差平均值归零，极低电平下仍然能听到原始信号（只是带一点嘶嘶声）。

**Triangular vs Rectangular**：矩形分布的方差 = 1/12，三角分布的方差 = 1/6（两倍），听感上三角 dither 的噪声更"温和"且无周期性 pattern。

**Highpass 变种**：再把三角噪声做一阶差分 `D[n] = U[n] - U[n-1]`，频谱变成 `|2·sin(πf/fs)|²`，**低频几乎没噪声，噪声集中在 fs/2 附近**。人耳对 15k+ 很迟钝，等效于"听起来更安静的 dither"。

**Noise shaping** (`SWR_DITHER_NS`)：更进一步，用一个 FIR 噪声滤波器（`ns_taps = 20`，`ns_coeffs`）把量化误差按"反等响度曲线"塑形，CD 母带压制用得多。

---

### Q19. `copy_pcm` / `copy_audio_interleave_float` 之类 SIMD 内部函数？

这类函数都由 `_template.c` 生成。关键文件：

| 模板 | 生成内容 |
|---|---|
| `libswresample/audioconvert.c` + `audioconvert_template.c` | 样本格式转换（`fltp → s16p` 等）的标量 C 版 |
| `libswresample/x86/audio_convert.asm` | x86 SIMD 版（SSE2/AVX/AVX2） |
| `libswresample/x86/resample.asm` + `resample_mmx.h` | x86 SIMD resample 内核 |
| `libswresample/aarch64/audio_convert.S` | ARM64 NEON |
| `libswresample/rematrix_template.c` | mix_1_1 / sum2 / mix_any 的 s16/s32/flt/dbl 四实例化 |
| `libswresample/resample_template.c` | `resample_common_TMPL` / `resample_linear_TMPL` 的 s16/s32/flt/dbl 实例化 |
| `libswresample/dither_template.c` | dither 路径 |

`audioconvert.c` 里用宏 `#include "audioconvert_template.c"` 多次实例化，定义 `CONV_FUNC(ofmt, otype, ifmt, expr)`。最终得到 `set_##ofmt##_from_##ifmt()` 这种函数，CPU 检测命中时替换成 `_x86 / _arm / _aarch64` 的 SIMD 版本。

面试加分：**为什么 planar fltp → packed s16 的 SIMD 收益最大**——一条 `vcvtps2dq + vpackssdw` 就能把 8 个 float 变 8 个 s16，还顺便做 interleave，吞吐远超标量。s16p → fltp 也差不多（`vpmovsxwd + vcvtdq2ps`）。

---

### Q20. 为什么 `swr_convert(NULL, ...)` 返回 `AVERROR(EINVAL)` 是安全的？

严格说——**`swr_convert(NULL, ...)` 会直接传 NULL 给 `s`，而函数第一步就 `s->in`，立刻 segfault**。libswresample 对 NULL ctx 的容错很弱（不像 `avcodec_free_context` 做了 `!*p` 判空）。

题目本意是：**`swr_convert(s, NULL, 0, NULL, 0)` 和 `swr_convert(s, NULL, out_count, NULL, 0)` 的语义**：

对比 `avcodec_send_packet(ctx, NULL)`：后者是"进入 draining 状态"的**信号**，意味着没有更多输入。

而在 `swr_convert`（`swresample.c:725`）里：
- `in_arg == NULL` 触发 `resampler->flush`，和 `send_packet(NULL)` 类似——进入 flush 模式，之后每次调用都尽量把内部缓冲吐出来。
- `out_arg == NULL && in_arg == NULL` 时，函数会 fallthrough 到 `fill_audiodata` 阶段，`out` 的 `ch[i]` 全是 NULL，真正尝试写入时会 crash。**所以"`out_arg == NULL` 本身不是 flush 信号，`in_arg == NULL` 才是**。

**对比区别**：

| API | `NULL` 的含义 | 后续行为 |
|---|---|---|
| `avcodec_send_packet(ctx, NULL)` | 进入 drain 模式，无法再 send | 只能 `receive_frame` 直到 `AVERROR_EOF` |
| `swr_convert(s, out, out_count, NULL, 0)` | flush 请求 | 照常返回样本数，可以重复调用直到 0 |
| `swr_convert(s, NULL, _, _, _)` | 非法（会 crash） | 没有语义 |

工程意义：swresample 的 flush 是"幂等的"——反复调用 `swr_convert(s, out, out_count, NULL, 0)` 直到返回 0 就算排空。不需要像 codec 那样维护"drain state machine"。

---

### 3H. 采样格式 / 通道 / 时间基（Q21–Q35）

### Q21. `AV_SAMPLE_FMT_*` 全表

`libavutil/samplefmt.h:55-72` 定义，按 enum 顺序（`libavutil/samplefmt.c:36-49` 的表）：

| enum | 名称 | bits | planar | altform |
|---|---|---|---|---|
| 0 | `U8` | 8 | no | `U8P` |
| 1 | `S16` | 16 | no | `S16P` |
| 2 | `S32` | 32 | no | `S32P` |
| 3 | `FLT` | 32 | no | `FLTP` |
| 4 | `DBL` | 64 | no | `DBLP` |
| 5 | `U8P` | 8 | yes | `U8` |
| 6 | `S16P` | 16 | yes | `S16` |
| 7 | `S32P` | 32 | yes | `S32` |
| 8 | `FLTP` | 32 | yes | `FLT` |
| 9 | `DBLP` | 64 | yes | `DBL` |
| 10 | `S64` | 64 | no | `S64P` |
| 11 | `S64P` | 64 | yes | `S64` |
| 12 | `NB` | — | — | — |

注意：`U8` 是**无符号**（偏移二进制，静音 = 0x80），其他整数格式都是有符号。浮点格式 `[-1.0, 1.0]` 为满幅，超出是 overload。

---

### Q22. `av_get_bytes_per_sample` 对每种格式的返回值？

`samplefmt.c:108-112`：`bits >> 3`。

| fmt | 返回 |
|---|---|
| `U8 / U8P` | 1 |
| `S16 / S16P` | 2 |
| `S32 / S32P` | 4 |
| `FLT / FLTP` | 4 |
| `DBL / DBLP` | 8 |
| `S64 / S64P` | 8 |
| 越界 | 0 |

**注意 `FLT == S32` 都是 4 字节**，区分靠枚举值而非大小。

---

### Q23. `AV_SAMPLE_FMT_S64P` 存在吗？

**存在**。见 `samplefmt.h:68-69`：

```c
AV_SAMPLE_FMT_S64,   ///< signed 64 bits
AV_SAMPLE_FMT_S64P,  ///< signed 64 bits, planar
```

`av_get_bytes_per_sample(AV_SAMPLE_FMT_S64P) == 8`。

用途：极少。主要给 `lavfi` 里的高精度中间计算（如 `asetnsamples` / `aresample` 链中的混音累加器）用。普通编解码器不吐 s64。

历史：**S64 / S64P 是 10、11**，在 `FLTP / DBLP` 之后加的，这意味着 `enum` 值顺序是"先 packed 四个 + planar 四个 + S64 + S64P"，不完全对称，写 switch 时不要假设 "packed 总在 < N，planar 总在 ≥ N"。

---

### Q24. `AV_SAMPLE_FMT_NB` 的作用？

`samplefmt.h:71`：

```c
AV_SAMPLE_FMT_NB   ///< Number of sample formats. DO NOT USE if linking dynamically
```

编译期枚举数量标志。用途：
1. 静态数组大小：`static const SampleFmtInfo sample_fmt_info[AV_SAMPLE_FMT_NB]`。
2. 边界检查：`if (fmt < 0 || fmt >= AV_SAMPLE_FMT_NB) return NULL;`
3. `for (int i = 0; i < AV_SAMPLE_FMT_NB; i++)` 遍历所有格式。

**"DO NOT USE if linking dynamically"** 警告：因为 FFmpeg 未来可能往枚举末尾加新格式（S64 就是这么加的），如果你的 app 动态链接 libavutil，用编译时的 `AV_SAMPLE_FMT_NB` 作为常量会和运行时的实际值不符，导致越界。安全做法：**别依赖它是特定值**，只当它是循环终点。

---

### Q25. `av_sample_fmt_is_planar` 和 `av_get_alt_sample_fmt`？

`samplefmt.c:114-119`：

```c
int av_sample_fmt_is_planar(enum AVSampleFormat sample_fmt) {
    if (sample_fmt < 0 || sample_fmt >= AV_SAMPLE_FMT_NB) return 0;
    return sample_fmt_info[sample_fmt].planar;
}
```

`samplefmt.c:68-75`：

```c
enum AVSampleFormat av_get_alt_sample_fmt(enum AVSampleFormat sample_fmt, int planar) {
    if (sample_fmt_info[sample_fmt].planar == planar)
        return sample_fmt;
    return sample_fmt_info[sample_fmt].altform;
}
```

- `av_get_alt_sample_fmt(S16, 1)` → `S16P`
- `av_get_alt_sample_fmt(S16P, 0)` → `S16`
- `av_get_alt_sample_fmt(FLTP, 1)` → `FLTP`（已经 planar，原样返回）

**便捷包装**：`av_get_packed_sample_fmt(fmt)` = `av_get_alt_sample_fmt(fmt, 0)`，`av_get_planar_sample_fmt(fmt)` = `av_get_alt_sample_fmt(fmt, 1)`。

---

### Q26. `av_samples_get_buffer_size` 的 `align` 参数对内存布局的影响？

`samplefmt.c:121-151`：

```c
if (!align) {  // align == 0: auto
    if (nb_samples > INT_MAX - 31) return AVERROR(EINVAL);
    align = 1;
    nb_samples = FFALIGN(nb_samples, 32);
}
line_size = planar ? FFALIGN(nb_samples * sample_size,               align)
                   : FFALIGN(nb_samples * sample_size * nb_channels, align);
return planar ? line_size * nb_channels : line_size;
```

三种调用方式：

| `align` 参数 | 含义 |
|---|---|
| `0` | **默认对齐**：`nb_samples` 先补到 32 的倍数，再按 `align=1` 不做字节对齐。32 是历史上 AVX 的 float 数（8 float × 4 字节 = 32 字节） |
| `1` | **无对齐**：按字面 `nb_samples * bps` 算 |
| `N > 1` | **按 N 字节对齐 line_size**（不改 nb_samples） |

区别：
- `align = 0` 多分配一点（最多 31 个样本）让 SIMD 读到 buffer 尾部不会越界。
- `align = 1` 精确分配，适合"按 sample 数精算"的场景（例如存文件）。
- `align = 16/32` 指定字节对齐，让每个 plane 起始地址对齐 SIMD 要求。

对 planar 格式，`av_samples_alloc` 返回的 buffer 总大小 = `line_size × nb_channels`，`data[0..N-1]` 指向一块连续内存的 N 段偏移。

---

### Q27. `AVChannelLayout` 四种 `order` 的数据区别？

见 `libavutil/channel_layout.h:114-160`：

| order | 数据存储 | 典型值 | 用途 |
|---|---|---|---|
| `UNSPEC` | 只有 `nb_channels`，其余字段未定义 | ADTS AAC 头里只有通道数，不知道谁是 L 谁是 R | 兼容 container 不带布局信息的情况 |
| `NATIVE` | `u.mask`（64 位），bit i 代表 `AVChannel` enum 第 i 位出现 | `AV_CH_LAYOUT_STEREO = AV_CH_FRONT_LEFT \| AV_CH_FRONT_RIGHT` | 95% 情况，标准电影/音乐布局 |
| `CUSTOM` | `u.map[]`：`nb_channels` 长的 `AVChannelCustom` 数组，每个元素有 `id / name / opaque` | 16+ 通道的影院系统、dual-mono、带自定义名字的通道 | 自由度最高，需要手动 alloc/free map |
| `AMBISONIC` | ACN 序，第 n 通道是 `l = floor(sqrt(n)), m = n - l*(l+1)` 的球谐函数（SN3D 归一化）。可选 `u.mask` 表示非 diegetic 通道 | VR 全景声 | 一阶 ambisonic = 4 通道 (W/Y/Z/X)，二阶 = 9 通道，三阶 = 16 通道 |

**数据区别**：
- `UNSPEC / NATIVE / AMBISONIC` 的 `u` 都是简单 `u.mask`（或未定义），结构体可以栈上分配、`memcpy` 拷贝、不需要 free。
- `CUSTOM` 的 `u.map` 是堆上数组（`av_malloc_array`），拷贝必须用 `av_channel_layout_copy`，销毁必须用 `av_channel_layout_uninit`。

---

### Q28. AMBISONIC 通道布局如何表示？

`channel_layout.h:134-155`：

> The audio is represented as the decomposition of the sound field into spherical harmonics.
> Channels are ordered according to ACN (Ambisonic Channel Number).
> `l = floor(sqrt(n)), m = n - l * (l + 1)`.
> Normalization is assumed to be **SN3D** (Schmidt Semi-Normalization) as defined in AmbiX $2.1.

- **ACN 顺序**：`n=0 → (l=0,m=0)=W`；`n=1 → (l=1,m=-1)=Y`；`n=2 → (l=1,m=0)=Z`；`n=3 → (l=1,m=1)=X`；...
- **SN3D 归一化**：不同于更老的 N3D 和 FuMa，SN3D 是 AmbiX 文件格式的标准。
- 阶数 `L` 和通道数 `N` 关系：`N = (L+1)²`。一阶 4，二阶 9，三阶 16，...

数据存储：`AVChannelLayout.order = AV_CHANNEL_ORDER_AMBISONIC`, `nb_channels = (L+1)²`, `u.mask` **可选**使用——用来标记"非 diegetic"辅助通道（如一阶 Ambisonic 之后再跟 2 个 head-locked 立体声通道）：

```c
AVChannelLayout amb = {
    .order = AV_CHANNEL_ORDER_AMBISONIC,
    .nb_channels = 4,
    // .u.mask = 0,  // 纯 ambisonic，无非 diegetic
};
```

加一个 stereo 非 diegetic：`nb_channels = 6, u.mask = AV_CH_LAYOUT_STEREO`。

---

### Q29. `av_channel_layout_index_from_channel` 和 `av_channel_layout_channel_from_index`？

声明在 `libavutil/channel_layout.h`（约 650 行前后）：

```c
int av_channel_layout_index_from_channel(const AVChannelLayout *channel_layout, enum AVChannel channel);
enum AVChannel av_channel_layout_channel_from_index(const AVChannelLayout *channel_layout, unsigned int idx);
```

对应关系：
- `index_from_channel(L_5P1, AV_CHAN_FRONT_CENTER)` → 在 5.1 中 FC 的位置，返回 `2`（假设 `FL=0, FR=1, FC=2, LFE=3, BL=4, BR=5`）。
- `channel_from_index(L_5P1, 3)` → `AV_CHAN_LOW_FREQUENCY`。

**为什么需要这两个函数？** 因为 `data[i]` 是**按顺序**的 plane 指针，你要知道"plane[2] 存的是哪个通道"就得用 `channel_from_index`。反过来，"我想把 FC 静音"先要 `index_from_channel` 拿到 plane 下标。

三种 order 都支持：
- `NATIVE`：遍历 `u.mask`，返回该 bit 是第几个 set。
- `CUSTOM`：查 `u.map[i].id`。
- `AMBISONIC`：ACN 算法计算。
- `UNSPEC`：返回 `AVERROR(EINVAL)` 或 `AV_CHAN_NONE`。

---

### Q30. `AV_CH_LAYOUT_5POINT1` 和 `AV_CH_LAYOUT_5POINT1_BACK` 的区别？

`channel_layout.h:227-230`：

```c
#define AV_CH_LAYOUT_5POINT0      (AV_CH_LAYOUT_SURROUND|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT)
#define AV_CH_LAYOUT_5POINT1      (AV_CH_LAYOUT_5POINT0|AV_CH_LOW_FREQUENCY)
#define AV_CH_LAYOUT_5POINT0_BACK (AV_CH_LAYOUT_SURROUND|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT)
#define AV_CH_LAYOUT_5POINT1_BACK (AV_CH_LAYOUT_5POINT0_BACK|AV_CH_LOW_FREQUENCY)
```

| 宏 | 通道集合 | 环绕位置 |
|---|---|---|
| `5POINT1` | FL, FR, FC, LFE, **SL, SR** | 环绕在**侧面**（90°） |
| `5POINT1_BACK` | FL, FR, FC, LFE, **BL, BR** | 环绕在**后面**（135°） |

**物理区别**：
- **Side**（SL/SR）：听众左右两侧，适用于现代 ITU-R BS.775 推荐的家庭影院布置（DTS、电影院标准）
- **Back**（BL/BR）：听众后方，早期 5.1 标准（AC-3/Dolby Digital、PAL DVD）

**工程影响**：不同 mask，在 rematrix 时走不同分支。例如 5.1_side → stereo 和 5.1_back → stereo 的系数其实一样（都用 `slev × M_SQRT1_2`），但如果你做 5.1 → 7.1（加回 side 或 back）就很不同：从 5.1_BACK 升到 7.1 时，side 通道从哪里来是关键问题。

错误混用后果：AAC 的 5.1 是 "FC, FL, FR, SL, SR, LFE" 顺序——注意 side，用 `5POINT1` mask。Dolby AC3 的老片可能是 back。如果把解出的 5.1_BACK 当成 5.1 去 rematrix，rematrix 矩阵会走错分支，产生错误的混音位置。

---

### Q31. `pts_wrap_bits` 和 MPEG-TS 33 位时间戳环绕？

`AVStream::pts_wrap_bits`：有些 container 的 PTS 不是单调递增的 64 位数，而是**循环计数器**。MPEG-TS 的 PTS 是 **33 bit**（定义在 ISO 13818-1，时钟 90 kHz），到 `2^33 - 1` 后回到 0。

33 bit @ 90 kHz 时长：`2^33 / 90000 / 3600 ≈ 26.5` 小时。

`libavutil/mathematics.h` 提供：

```c
int64_t av_compare_mod(uint64_t a, uint64_t b, uint64_t mod);
```

比较模 `mod` 下 a 和 b 的"环形差值"（类似 TCP sequence number 的 wrap-around compare）。如果 `a - b` 在 `[-mod/2, mod/2]` 之间，返回正负号。

用途：demuxer 读到 `pts = 100`，上一次是 `pts = 2^33 - 50`。naive 比较会以为倒退了，但 `av_compare_mod(100, 2^33-50, 2^33) > 0`——正向推进了 150。

**坑**：长录制（超过 26.5 小时的直播）或者快速 seek 后，如果代码用普通 `int64_t` 比较 PTS 会把"大 pts"当成"更新"，从而错乱。FFmpeg 内部用 `pts_wrap_bits` 字段指导 `av_compare_mod` 解决。大多数 mp4/mkv 是 64 位无 wrap，`pts_wrap_bits = 64`。

---

### Q32. `av_rescale_q` vs `av_rescale_q_rnd`？

`libavutil/mathematics.h:196-214`：

```c
int64_t av_rescale_q    (int64_t a, AVRational bq, AVRational cq);
int64_t av_rescale_q_rnd(int64_t a, AVRational bq, AVRational cq, enum AVRounding rnd);
```

- `av_rescale_q(a, bq, cq)` = `a * bq / cq`，等价于 `av_rescale_q_rnd(..., AV_ROUND_NEAR_INF)`。
- `av_rescale_q_rnd` 允许指定 rounding mode。

`enum AVRounding`：

| mode | 含义 |
|---|---|
| `AV_ROUND_ZERO` | 向零取整（截断） |
| `AV_ROUND_INF` | 远离零 |
| `AV_ROUND_DOWN` | 向下（floor） |
| `AV_ROUND_UP` | 向上（ceil） |
| `AV_ROUND_NEAR_INF` | 就近，等距时远离零（默认） |
| `\|AV_ROUND_PASS_MINMAX` | 保留 `INT64_MIN/MAX` 哨兵值 |

对 PTS 精度的影响：
- 通常 PTS 转换 stream_tb → codec_tb（或反之）用默认 `NEAR_INF` 足够。
- 分配输出 buffer 的"安全上界"要用 `AV_ROUND_UP`，避免下取整导致 buffer 短 1 样本。例如 `av_rescale_rnd(delay + in_samples, out_rate, in_rate, AV_ROUND_UP)`。
- 视频 `TIME_BASE` 计算 duration 时，如果连续几帧都 `NEAR_INF` 取整，累计误差可能偏 ±1 tick 漂移。严格的"帧边界对齐"要用 `av_add_stable`。
- `AV_ROUND_DOWN` 用于 seek 目标时间戳——希望落到"不超过"目标的关键帧。

**常见 bug**：分配输出 buffer 时用 `av_rescale_q` (= `NEAR_INF`)，恰好碰到 .5 被舍入到偶数，buffer 差 1 样本 → swresample 写溢出。正确做法：`av_rescale_rnd(..., AV_ROUND_UP) + 1`（额外留一个 safety margin）。

---

### Q33. `av_add_stable` 为什么存在？

`libavutil/mathematics.h:289`：

```c
int64_t av_add_stable(AVRational ts_tb, int64_t ts, AVRational inc_tb, int64_t inc);
```

用途：往一个 `ts_tb` 时间基下的时间戳 `ts` 上，稳定地加 `inc_tb * inc`。

**为什么不能直接 `ts + av_rescale_q(inc, inc_tb, ts_tb)`？**

假设帧率 48000/1001，duration_per_frame 在 `tb = 1/90000` 下不是整数：
```
90000 * 1001 / 48000 = 1876.875
```

每帧加 `round(1876.875) = 1877`，连续加 1000 帧后累计误差 `1000 * 0.125 = 125` tick = 1.4 ms 漂移。

`av_add_stable` 的做法：**不累加 delta，而是按"总帧数 × duration"整体重新 rescale 一次**。它把上一次的 `ts` 记为"N 帧时的位置"，这次加 inc 后变成 N+1 帧，**重新按 `(N+1) * inc_tb` 整体计算 ts**。每次重算都从初值出发，误差不会累加。

面试答法：**它存在是为了消除浮点/整数累加的漂移**。音频流尤其重要——44.1kHz 在 `{1, 14112000}` 下每帧 `14112000/44100 = 320` 是整数，无问题；但 48kHz 在 `{1, 90000}` 下每帧 `90000/48000 = 1.875` 不是整数，必须用 `av_add_stable`。

---

### Q34. 14112000 = LCM(8k, 11.025k, 12k, 16k, 22.05k, 24k, 32k, 44.1k, 48k) 验算

先把所有采样率分解：

| rate | 因式分解 |
|---|---|
| 8000 | 2^6 × 5^3 |
| 11025 | 3^2 × 5^2 × 7^2 |
| 12000 | 2^5 × 3 × 5^3 |
| 16000 | 2^7 × 5^3 |
| 22050 | 2 × 3^2 × 5^2 × 7^2 |
| 24000 | 2^6 × 3 × 5^3 |
| 32000 | 2^8 × 5^3 |
| 44100 | 2^2 × 3^2 × 5^2 × 7^2 |
| 48000 | 2^7 × 3 × 5^3 |

LCM 取每个素数的最高幂次：
- 2 的最高幂：`2^8`（来自 32000） = 256
- 3 的最高幂：`3^2`（来自 11025/22050/44100） = 9
- 5 的最高幂：`5^3`（来自 8k/12k/16k/24k/32k/48k） = 125
- 7 的最高幂：`7^2`（来自 11025/22050/44100） = 49

`LCM = 256 × 9 × 125 × 49 = 256 × 9 × 6125`
- `256 × 9 = 2304`
- `2304 × 6125 = ?`
- `2304 × 6000 = 13,824,000`
- `2304 × 125 = 288,000`
- 合计 `14,112,000` ✓

验算整除：
- `14112000 / 8000 = 1764` ✓
- `14112000 / 11025 = 1280` ✓
- `14112000 / 44100 = 320` ✓
- `14112000 / 48000 = 294` ✓

这就是为什么 AVCodecContext 的音频 `time_base` 常用 `{1, 14112000}`——所有标准音频采样率下每个样本对应的时间都是整数 tick，avoids 浮点误差。

---

### Q35. 48000 vs 44100 两帧 duration 对应的 pts step

`time_base = {1, 14112000}` 下：

| sample_rate | 每 sample 的 pts step | 1024-sample 帧 pts step | 1152-sample 帧 pts step |
|---|---|---|---|
| 48000 | `14112000 / 48000 = 294` | `294 × 1024 = 301,056` | `294 × 1152 = 338,688` |
| 44100 | `14112000 / 44100 = 320` | `320 × 1024 = 327,680` | `320 × 1152 = 368,640` |

对应题目所说**单样本 pts step**：**48000 → 294, 44100 → 320**。（注意直觉上采样率高的 pts step 反而小——因为 1 个样本对应的时间更短。）

一帧 AAC（1024 samples）@ 48k：`1024 / 48000 = 21.333...` ms，pts step = 301056 tick。

**关键点**：这两个 step 都是**精确整数**，不会漂移，可以无累计误差地加 N 帧。这就是为什么 FFmpeg 用 14112000——它让 44.1k 和 48k 以及它们的半采样率（22.05k、24k、8k 等）都对齐到整数 tick，同一个 pipeline 混合处理不同采样率的音频不会因为 rescale 引入半个 tick 误差。

如果用 `time_base = {1, 90000}`（视频的 90kHz 基），48k 每样本是 `90000/48000 = 1.875` tick，需要 `av_add_stable` 才能稳定累加；44.1k 是 `90000/44100 = 2.04...`，更糟。这就是 "音频专用时间基" 的存在理由。

---

### 3I. 底层音频基础设施 preview（Q36–Q40）

### Q36. SDL2 `SDL_QueueAudio` 和 audio callback 模式的 push/pull 差异？

SDL2 提供两种音频输出模式：

| 模式 | API | 方向 | 线程关系 |
|---|---|---|---|
| **Callback（pull）** | `SDL_OpenAudioDevice` 时传 `callback` 函数指针；`SDL_PauseAudio(0)` 启动 | **SDL 拉**：SDL 音频线程回调你的函数，你必须在回调里**立即**填满 buffer | 回调在 SDL audio 线程，你的主线程要和回调用锁或无锁 ring buffer |
| **Queue（push）** | `SDL_OpenAudioDevice(callback = NULL)`；主线程 `SDL_QueueAudio(dev, data, len)` | **你推**：调用 `SDL_QueueAudio` 把 PCM 直接送进 SDL 内部 ring buffer | 无回调，SDL audio 线程自己从 queue 取 |

**实现本质差异**：
- Callback：SDL 暴露"数据消费时机"，调用方必须实时响应（通常 <10 ms），否则 underrun。适合"按 sample-accurate tick 生成" 的场景（软合成器、低延迟游戏）。
- Queue：SDL 内部多一层缓冲，调用方可以随时推数据，SDL 负责到 audio device 的 transfer。**实现简单**——调用方不需要写锁、不需要管理回调生命周期。`ffplay` 在新版本里用 Queue 模式，旧版本用 Callback。

**性能**：两者最终都是往 SDL 内部 ring buffer 写，只是"谁来驱动写入"。Callback 延迟低但耦合重；Queue 延迟稍高但解耦彻底。

**失败模式**：
- Callback 没填满 → 输出上下文"残留前一帧"或静音 pop。
- Queue 耗尽 → SDL 自动静音，`SDL_GetQueuedAudioSize` 可查深度。

---

### Q37. `SDL_AudioSpec::samples` 对延迟的影响？

`SDL_AudioSpec.samples` 是**每次 callback（或 device DMA 传输）处理的样本数**，必须是 2 的幂。

延迟公式：`latency ≈ samples / freq * buffer_count`

| samples | freq=44100 | 延迟（单缓冲） | 延迟（双缓冲） |
|---|---|---|---|
| 256 | | ~5.8 ms | ~11.6 ms |
| 512 | | ~11.6 ms | ~23.2 ms |
| 1024（默认） | | ~23.2 ms | ~46.4 ms |
| 2048 | | ~46.4 ms | ~92.9 ms |
| 4096 | | ~92.9 ms | ~185.8 ms |

SDL 通常是**双缓冲**（一块给 DMA 读，一块给 callback 填），真实延迟 ≈ `2 × samples / freq`。

权衡：
- **小 samples**：延迟低，但 callback 触发频繁，开销大，且对 CPU 响应时间要求高——callback 执行时间必须 << `samples/freq`，否则 underrun。
- **大 samples**：延迟高但 callback 间隔大，CPU 友好，不容易 underrun。

**ffplay 的取值**：`SDL_AUDIO_MIN_BUFFER_SIZE = 512`, `SDL_AUDIO_MAX_CALLBACKS_PER_SEC = 30`。根据 sample rate 动态计算：`samples = max(512, next_pow2(freq / 30))`，保证 callback 每秒 ≥30 次，平衡延迟和 CPU。

**驱动层的真实 buffer 可能更大**：SDL 内部的 `samples` 只是 callback 粒度，底层 CoreAudio/ALSA 可能还有自己的 ring buffer（通常 1–3 个周期）。

---

### Q38. macOS CoreAudio 的 `AudioQueueEnqueueBuffer` 和 SDL 的等价？

CoreAudio 的 Audio Queue Services（高层 API）：

```objc
AudioQueueNewOutput(&format, callback, userData, NULL, NULL, 0, &queue);
AudioQueueAllocateBuffer(queue, bufferSize, &buffer);
memcpy(buffer->mAudioData, pcm, len);
buffer->mAudioDataByteSize = len;
AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
AudioQueueStart(queue, NULL);
```

**等价对应**：

| CoreAudio (Audio Queue) | SDL2 |
|---|---|
| `AudioQueueNewOutput(..., callback, ...)` | `SDL_OpenAudioDevice(..., spec_with_callback, ...)` |
| `AudioQueueAllocateBuffer` | SDL 内部自动管理，不暴露 |
| `AudioQueueEnqueueBuffer` | `SDL_QueueAudio`（push 模式），或在 callback 里填 buffer（pull 模式） |
| `AudioQueueStart` | `SDL_PauseAudioDevice(dev, 0)` |
| `AudioQueueStop` | `SDL_PauseAudioDevice(dev, 1)` |

**模型差异**：
- CoreAudio Audio Queue 是**纯 push 模型**：你 allocate 几个 buffer（通常 3 个）→ 填数据 → enqueue → callback 通知"这个 buffer 播完了，你可以 refill 再 enqueue"。
- SDL 的 callback 模式更像**纯 pull**：SDL 告诉你"现在需要 N 个样本"，你立刻填进 SDL 给的 buffer。
- SDL 的 Queue 模式 ≈ CoreAudio Audio Queue 但更简化（SDL 自己管 buffer）。

**更低层的 CoreAudio**：`AudioUnit` API，绕开 Audio Queue 直接和 HAL (Hardware Abstraction Layer) 交互，延迟更低（<5 ms）但 API 复杂得多。FFmpeg 的 `coreaudio_enc.c` 用的就是 AudioUnit。

---

### Q39. ALSA 的 `snd_pcm_writei` 阻塞行为？

```c
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size);
```

`i` 后缀 = interleaved。对应的 planar 版本是 `snd_pcm_writen`。

**默认阻塞模式**：

1. 调用时 ALSA 把 `size` 样本写入**内部 ring buffer**（大小由 `snd_pcm_hw_params_set_buffer_size` 决定）。
2. 如果 ring buffer 空间不够（hardware DMA 还没消费完），`writei` **阻塞**直到有空位。
3. 返回实际写入的 frame 数（可能 < size）。

**非阻塞模式**：`snd_pcm_open(..., SND_PCM_NONBLOCK)` 或 `snd_pcm_nonblock(pcm, 1)`：
- buffer 满时立即返回 `-EAGAIN`，不阻塞
- 配合 `poll()` 监听 `POLLOUT` 事件

**状态机**（`snd_pcm_state_t`）：
- `SND_PCM_STATE_OPEN` / `PREPARED`：准备好但未 start
- `SND_PCM_STATE_RUNNING`：DMA 在跑
- `SND_PCM_STATE_XRUN`：**underrun** 了（ring buffer 空了，硬件找不到数据）——`writei` 返回 `-EPIPE`，必须调 `snd_pcm_prepare` 重置才能继续写
- `SND_PCM_STATE_SUSPENDED`：系统挂起（`-ESTRPIPE`），调 `snd_pcm_resume`

**典型调用模式**：

```c
while (pending) {
    frames = snd_pcm_writei(pcm, buf, chunk);
    if (frames < 0) frames = snd_pcm_recover(pcm, frames, 0);  // 自动处理 XRUN
    if (frames < 0) break;  // fatal
    buf += frames * frame_size;
    pending -= frames;
}
```

**和 SDL 比较**：ALSA 是底层 API，直接和 kernel DMA buffer 打交道，一次 `writei` 等价于 "`memcpy` 到 ring buffer"。没有 callback 概念（高层 `snd_pcm_async_*` 提供 signal 回调，但极少用）。ffmpeg 的 `alsa.c` 走的就是纯阻塞 `writei` + 非阻塞 poll 两种模式可选。

---

### Q40. 为什么多数音频 API 都是"push"到 ring buffer，底层驱动用 DMA 拉走？

这是**硬件架构决定的抽象**。从下往上看：

**1. 硬件层**：声卡/USB audio device 通过 **DMA** 从一块物理内存 buffer 读样本，按固定速率 (sample clock) 发给 DAC。DMA 的"pull"方向是硬性的——硬件时钟不等 CPU。

**2. 内核驱动层**：暴露 ring buffer 给用户态。驱动的任务是：
- 保证 DMA 始终有数据读（不能 underrun，否则声音咔嚓）
- 提供"我消费到哪儿了"的指针（`hw_ptr`）
- 接收用户态的"我写到哪儿了"（`appl_ptr`）

**3. 用户态 API**（ALSA/CoreAudio/WASAPI/SDL/PulseAudio）：两种包装策略

| 策略 | 代表 | 机制 |
|---|---|---|
| **Push**（你推入 ring） | ALSA `writei`、SDL `QueueAudio`、WASAPI `IAudioRenderClient::GetBuffer` + `ReleaseBuffer` | 调用方主动写数据到 buffer，驱动的 DMA 自己 pull。底层是 ring buffer 生产者。 |
| **Pull**（callback 拉数据） | CoreAudio Audio Unit、SDL callback mode、JACK、PulseAudio write callback | API 在 audio 线程回调你"给我 N 样本"，你必须立刻产生。本质上 callback 函数也是在"往 ring buffer 写"，只是由 API 调度时机。 |

**为什么底层一定是 push 到 ring buffer**：
1. **硬件是 pull**：DMA 按时钟节拍拉数据，停不下来。必须有个"随时可读的"buffer 挂在 DMA 前面。
2. **CPU 是异步**：应用线程不可能精确按 sample clock 生成数据，必须攒一小块再交。
3. **buffer 是唯一方案**：让 CPU 侧 "批量生产"、让 DMA 侧 "匀速消费"，中间用 ring buffer 解耦两个时钟域。

**pull/push 只是调用约定**：
- "Push" 把生产时机控制权给应用（应用决定什么时候写数据，可能一次写很多）。
- "Pull" 把生产时机控制权给系统（系统决定什么时候要数据，应用必须守候）。
- 但它们最终都是在填同一种 ring buffer，**只是触发写入的"谁"不同**。

**工程含义**：
- 低延迟实时合成 → 用 pull（CoreAudio Audio Unit、JACK），让 callback 时机和 DMA tick 对齐。
- 播放器（ffplay、VLC）→ 用 push（SDL QueueAudio、ALSA writei），decoder 按自己的节奏解码，攒一批写一批。
- 推/拉不会影响底层延迟下限（下限是 DMA ring buffer 大小），只影响代码组织方式和调用方的实时性要求。

---

### 3G/3H/3I 自检清单

- [x] 所有代码引用可回源到 `libswresample/*.c` / `libavutil/*.c/*.h`（commit `554dcc2885`）
- [x] 5.1 下混系数来自 `rematrix.c:154-165, 214-228`，DPL2 特殊相位系数在 `215-224`
- [x] s16p rematrix 量化是 Q15（`32768` 标量，见 `rematrix.c:479`）—— 题目原文的"30 位 for s32"是陷阱，源码实际也是 Q15
- [x] `build_filter` 行号 `resample.c:41-131`，flush padding 行号 `437-454`
- [x] `AV_SAMPLE_FMT_S64P` 存在且 `av_get_bytes_per_sample` 返 8（`samplefmt.c:46`）
- [x] `AVChannelLayout` 四种 order 定义 `channel_layout.h:114-160`
- [x] 14112000 的因式分解验算整数：`1764 / 1280 / 320 / 294` 全部整除
- [x] `time_base = {1, 14112000}` 下每样本 pts step：48k → 294，44.1k → 320（正比于 `14112000 / rate`）
### 3J. 应用层 bug（Q1-Q20）

---

**Q1. 循环解码一个 MP3，每次 `avcodec_receive_frame` 出来后直接 memcpy `frame->data[0]`，循环头忘了 `av_frame_unref(frame)`。程序看起来能跑，但跑几分钟后内存无限涨，有时 `frame->data[0]` 指向的是上一帧 planar 布局的地址。问题出在哪？**

**缺陷**：`AVFrame` 内部持有多个 `AVBufferRef`（`buf[0..7]` 和 `extended_buf`）。decoder 的 `receive_frame` 发现 frame 非空时，并不会替你释放旧 refs——而是**直接覆写** `data`/`linesize`/`extended_data` 字段。旧的 `AVBufferRef` 仍然指向上一帧的 refcounted buffer，refcount 永远不减。

**影响**：每次循环泄漏一个 decoder 输出 buffer（MP3 一帧 1152 samples × 4 字节 × 2 声道 ≈ 9 KB）。24 kHz 播 1 小时泄漏接近 1 GB。更阴险的是：planar 格式下 `data[1]` 可能仍指向旧 buffer 末端偏移，而 `nb_samples` 是新的——读到野数据。

**修复**：
```c
while (av_read_frame(fmt, pkt) >= 0) {
    avcodec_send_packet(dec, pkt);
    while (avcodec_receive_frame(dec, frame) == 0) {
        use(frame);
        av_frame_unref(frame);  // 必须在循环内
    }
    av_packet_unref(pkt);
}
```

**教训**：所有 "out-ref" API（`receive_frame`、`av_read_frame`）的契约都是"调用时 frame/pkt 必须是空的或已 unref 的"。FFmpeg 不会替你清理你持有的引用。

---

**Q2. 一个 demuxer 循环每次调 `av_read_frame(fmt, &pkt)`，但不调 `av_packet_unref`。第二次调用居然成功返回，但 pkt 的数据混乱。为什么？**

**缺陷**：`av_read_frame` 内部做了"兼容旧代码"的 side-effect——进入函数时如果发现 `pkt->buf != NULL`，会先 `av_packet_unref`，再填入新的 packet。所以看似"忘了 unref 也没事"。

**影响**：
1. 每次循环头的隐式 unref 依赖**调用者先初始化 pkt 为零**。栈上 `AVPacket pkt;` 未 `av_init_packet`（6.x 已废弃）或 `av_packet_alloc` 时，`pkt->buf` 是随机栈值，隐式 unref 跳过 → 真正泄漏，而且 pkt 字段残留上一次的 `stream_index`、`pts`。
2. 如果用户保留旧 pkt 的副本（例如压入自己的队列），第二次 `av_read_frame` 会把它的 `buf` 给 unref 掉，导致队列里对应的 packet 指针悬挂。

**修复**：始终用 `av_packet_alloc` 分配，循环末尾主动 `av_packet_unref`。入队时用 `av_packet_clone` 或 `av_packet_move_ref`。

**教训**：FFmpeg "方便"的隐式 unref 是兼容遗物，依赖它写出的代码在队列/异步场景必坏。

---

**Q3. `swr_convert` 的输出缓冲按 `out_nb_samples = frame->nb_samples * out_rate / in_rate` 算，不加上 `swr_get_delay`。短文件能跑，但转 1 小时 44.1k→48k 的音频后发现尾部 PCM 被截断，且中段偶发 `Output buffer too small`。为什么？**

**缺陷**：libswresample 内部有**多相滤波器延迟缓冲区**（`filter_length / 2` 个样本）。真正可能吐出的样本数 = `swr_get_delay(ctx, in_rate) + in_nb_samples`，重采样后 = `av_rescale_rnd(delay + in, out_rate, in_rate, AV_ROUND_UP)`。忽略 `delay` 就低估输出大小。

**影响**：`swr_convert` 会尽量吐，遇到 out 不够会报错或截断到 out 大小，剩余样本留在延迟缓冲里——**下一次 convert 会先吐这些旧样本再吐新的**，表现为"尾部丢、中段挤压"。

**修复**：
```c
int64_t delay = swr_get_delay(swr, in_rate);
int out_count = av_rescale_rnd(delay + in->nb_samples, out_rate, in_rate, AV_ROUND_UP);
av_samples_alloc(&out_buf, NULL, out_ch, out_count, AV_SAMPLE_FMT_S16, 0);
int got = swr_convert(swr, &out_buf, out_count, in->extended_data, in->nb_samples);
```

**教训**：swresample 是**有状态的**。输出大小必须按"延迟缓冲 + 新输入"计算，永远留一点余量（+256 更保险）。

---

**Q4. 用 `av_samples_alloc(&buf, &linesize, 2, 1024, AV_SAMPLE_FMT_FLTP, 0)` 分配 planar 音频，释放时只写 `av_freep(&buf[0])`。planar 下是否泄漏？**

**缺陷**：`av_samples_alloc` 对 planar 的布局是**一次 `av_malloc`**，然后把 `buf[0]`、`buf[1]` 设成同一块内存内的偏移。`buf[0]` 就是 malloc 出来的原始指针。`av_freep(&buf[0])` 等效于 `av_free(buf[0]); buf[0] = NULL;`——这是对的，不泄漏。

**但是**：`buf` 本身如果是 `uint8_t **buf = NULL; av_samples_alloc(&buf, ...)` 这种需要先 `av_malloc_array(buf, planes)` 的双重指针场景下，`buf` 数组本身也要 `av_freep(&buf)`。`av_samples_alloc_array_and_samples` 才会自己 alloc 外层数组，相应地释放要 `av_freep(&buf[0]); av_freep(&buf);`。

**影响**：混淆这两个 API → 外层数组泄漏（每次 16 字节，不易被 leak detector 发现但长跑会爆）。

**修复**：配对地用。`av_samples_alloc_array_and_samples` 对称地 `av_freep(&buf[0]); av_freep(&buf);`；`av_samples_alloc` 用静态 `uint8_t *buf[8]` 只需释放 `buf[0]`。

**教训**：内外层所有权要对称。FFmpeg 的 "samples alloc" 系列有两个版本，差异就在**外层指针数组是否自带 malloc**。

---

**Q5. 两个线程共用一个 `SwrContext`，各自跑 `swr_convert`。偶发输出混入另一路的数据。**

**缺陷**：`SwrContext` 持有"延迟缓冲、rematrix 矩阵、上一次 sample 索引"等内部状态。`swr_convert` 是**有状态的流函数**，不是纯函数。两个线程交错调用 → 延迟缓冲里混入对方的样本。

**影响**：听感上是爆音/串音，尤其在不同采样率的两路音频之间最明显。fuzzer 抓不到，线上 bug 报告模糊。

**修复**：每个线程独立 `swr_alloc_set_opts2 + swr_init`。`SwrContext` 不共享。代价是几百字节到几 KB 的复制，可忽略。

**教训**：libswresample / libavcodec / libavformat 全家桶里，**除了 `av_*` 纯工具函数以外没有任何 context 是线程安全的**。文档说"构造好后可以只读"，但 convert/receive/send 都修改内部状态。

---

**Q6. 用户拖动进度条后 `av_seek_frame`，但没 `avcodec_flush_buffers`。MP3 输出听到"哒"一声，AAC 输出前 50ms 是爆音。为什么两个 codec 表现不同？**

**缺陷**：decoder 内部有**跨帧状态**：
- MP3：`synth_buf`（IMDCT overlap-add 缓冲）+ huffman 预测状态。seek 后第一帧的 IMDCT 要和"旧 overlap" 叠加——但旧 overlap 是 seek 前的位置，内容无关 → 第一帧前半段是两路声音的混叠。
- AAC LC：上面那些 + `AV_CODEC_CAP_DELAY` 声明的 SBR/PS 延迟缓冲（1-2 帧）。seek 后头 1024-2048 个样本是"旧上下文 + 新系数"的混叠，持续更久。

**影响**：MP3 是瞬态"哒"声，AAC 是几十毫秒爆音。

**修复**：seek 后立刻 `avcodec_flush_buffers(dec_ctx)`，decoder 内部清空 overlap 缓冲 + `AV_CODEC_FLAG2_SKIP_MANUAL` 丢掉 `delay` 之前的输出。

**教训**：seek 后必须 flush，这不是可选优化。MP3/AAC/Vorbis/Opus 都有 overlap-add，每个都需要。

---

**Q7. 把 `frame->extended_data[0]` 当成唯一 buffer 直接 memcpy 到播放队列。6 通道 5.1 听起来正常，7.1 声道立体少几个通道。24 通道 Ambisonic 直接 crash。**

**缺陷**：`AVFrame::data` 是固定 `uint8_t *data[8]`。`extended_data`：
- ≤8 通道：`extended_data == data`，`data[i]` 就是 plane i
- \>8 通道：`extended_data` 指向堆上额外分配的 `uint8_t **`，`data` 只存前 8 个

你只读 `extended_data[0]` 等于只读第一个 plane。对 planar 格式，其它通道全丢。对 7.1（8 通道）还能跑，对 Ambisonic（24 通道）到 `extended_data[9]` 以上时，你的代码可能越界访问了 `data[9]` 那块栈外空间。

**影响**：≤8 通道静默丢通道，>8 通道 crash 或野数据。

**修复**：
```c
int planes = av_sample_fmt_is_planar(fmt) ? ch : 1;
for (int p = 0; p < planes; p++)
    memcpy(dst + p * plane_size, frame->extended_data[p], plane_size);
```
**教训**：音频代码永远用 `extended_data`，永远按 `planes` 计算边界，不要硬编码到 `data[0..7]`。

---

**Q8. `printf("samplefmt=%s\n", av_get_sample_fmt_name(ctx->sample_fmt))`。在 Linux/glibc 下打印 "(null)" 能跑；在 macOS 上解码一个损坏的 AAC 文件时崩溃。**

**缺陷**：`av_get_sample_fmt_name` 的契约是"非法输入返回 `NULL`"。C 标准对 `printf("%s", NULL)` 是**未定义行为**：glibc 扩展会打印 `(null)`，macOS libSystem 和 musl 直接 SIGSEGV。损坏 AAC 流里 decoder 可能临时把 `sample_fmt` 设为 `AV_SAMPLE_FMT_NONE`（=-1），`av_get_sample_fmt_name` 返回 NULL。

**影响**：跨平台的定时炸弹。QA 在 Linux 发现不了，用户在 macOS 挂。

**修复**：
```c
const char *n = av_get_sample_fmt_name(sf);
printf("samplefmt=%s\n", n ? n : "unknown");
```

**教训**：FFmpeg 所有 `av_get_*_name` 函数对非法输入返回 NULL，要守卫。`AVCodec::long_name` 在 `--enable-small` 下也可能是 NULL——同理守卫。

---

**Q9. 旧代码写 `dec_ctx->channels = 2;`，新代码写 `dec_ctx->ch_layout.nb_channels = 2;`。FFmpeg 6.x 下混用，某些 encoder 初始化后声道顺序错。哪个字段是权威？**

**缺陷**：FFmpeg 5.1 起 `AVCodecContext::channels` + `channel_layout` 被标记 deprecated。**`ch_layout` 是权威来源**。旧字段仍然存在只为源码兼容，但写旧字段是否同步到新字段依赖 `FF_API_OLD_CHANNEL_LAYOUT` 编译宏——6.x 开始默认不同步。

**影响**：你设了 `channels=2` 没设 `ch_layout`，encoder 看到 `ch_layout.nb_channels == 0` → 初始化失败或按默认单声道处理。opus encoder 特别敏感，直接把声道顺序错乱。

**修复**：只设 `ch_layout`：
```c
av_channel_layout_copy(&ctx->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);
```
或用静态字面量：`ctx->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;`

**教训**：API 升级时永远不要新旧并存。5.1 及以后**只认 `ch_layout`**。

---

**Q10. 把 packet 入队列要用 `av_packet_ref` 还是 `av_packet_clone`？用 `ref` 的话共享 buffer，用 `clone` 的话多一次 alloc——实现上有什么差别？**

**缺陷**：两者都**共享**底层 data buffer（通过 refcount 加 1），区别在**外层 `AVPacket` 结构体**：
- `av_packet_ref(dst, src)`：dst 必须预先 alloc 好（`av_packet_alloc`），ref 只是把 src 的字段 + refcount 拷到 dst
- `av_packet_clone(src)`：内部 `av_packet_alloc` + `av_packet_ref`，返回新的 `AVPacket *`

如果你写队列 `queue.push(av_packet_ref(slot, src))` 但 slot 是未初始化的栈变量，`av_packet_ref` 会 assert 或者复用旧 buf 导致 UAF。

**影响**：长期泄漏或 crash。

**修复**：队列用 `AVPacket *`（不是 `AVPacket`），入队时 `av_packet_clone`。出队时 `av_packet_free`。对称。

**教训**："ref 要求 dst 已初始化"、"clone 内部 alloc" 是两个 API 分工不同的根本原因。混用一定错。

---

**Q11. 写 WAV 用 `out_size = frame->nb_samples * channels * sizeof(int16_t)` 计算要写入的字节数。在 7.1 声道某些 frame 上尾部被截断几个字节。**

**缺陷**：libswresample 输出给 planar 格式时会**按 `align` 对齐每个 plane**（默认 `align=32` 字节，用于 AVX2 向量加载）。`av_samples_get_buffer_size(&linesize, ch, nb_samples, fmt, 0)` 按 align=0 算"紧密" buffer 大小；align=1 算"对齐" buffer 大小。planar 下 `linesize > nb_samples * sample_bytes`，超出的部分是 padding。

如果你按 `nb_samples * ch * bytes` 算，对 packed 格式正确；但写 planar 下的每个 plane 时你应该写 `nb_samples * bytes`，而不是 `linesize`（会多写 padding）。反过来，如果你误用 `linesize * planes` 当"样本数据总大小"，尾部多写 padding，WAV 总长度错。

**影响**：WAV 尾部多几十字节垃圾或少几字节。播放器多半能忍，但 md5 对不上。

**修复**：
```c
int data_size = av_samples_get_buffer_size(NULL, ch, frame->nb_samples, fmt, 1);
```
`align=1` 表示"无对齐，紧密字节数"。

**教训**：planar 格式 linesize ≠ "plane 的有用字节数"，存在 align padding。永远用 `av_samples_get_buffer_size` 而不是手算。

---

**Q12. 把 MP3 解出的 `fltp` 格式直接送给 SDL2 `SDL_OpenAudioDevice(..., AUDIO_F32SYS, ...)`。听到的是：左右声道各自前半段是左声道信号、后半段是右声道信号，循环节奏诡异。**

**缺陷**：SDL 不支持 **planar** 格式，只接受 **packed/interleaved**。`fltp` 的内存布局是 `LLLL...RRRR...`。SDL 按 `LRLRLR...` 读，于是：
- 前 N/2 个 stereo 帧读到的"L"是真 L，"R"是真 L 的后半
- 后 N/2 个 stereo 帧读到的"L"是真 R 的前半，"R"是真 R 的后半

听感就是你描述的节奏错位。

**影响**：完全不可用。

**修复**：swresample 把 `fltp` → `flt`（packed）：
```c
swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, 44100,
                    &in_layout,  AV_SAMPLE_FMT_FLTP, 44100, 0, NULL);
```

**教训**：SDL / 多数播放后端只吃 packed。planar 是 FFmpeg 内部为了 SIMD 优化选的存储形式。decoder 输出 planar 必须 resample 成 packed。

---

**Q13. 解码 + `SDL_QueueAudio` 循环没做 backpressure。慢消费者场景下队列积压到 500 MB，程序 OOM。**

**缺陷**：`SDL_QueueAudio` 内部是**无界 FIFO**，你喂多快它收多快。decoder 是 CPU-bound 但非常快（MP3 20ms 一帧解码只要几百 μs），实时播放每 20ms 才消耗一帧。二者速率差 ~100 倍 → 每秒积压 100 倍。

**影响**：OOM；即便不 OOM，拖动进度条时也要等几十秒队列清空才听到新位置。

**修复**：生产者检查 `SDL_GetQueuedAudioSize(dev) < HIGH_WATER`（例如 200 ms 的字节数），否则 `SDL_Delay(5)` 退让。或者用 `SDL_AudioDeviceAudioStream` API 直接限界。

**教训**：任何异步播放 API 都要 backpressure。"只要喂得够快"的思路是错的。

---

**Q14. `av_rescale_q(pts, {1,14112000}, {1,44100})` 没指定 rounding，默认 `AV_ROUND_NEAR_INF`。连续算 10000 个 frame 后发现输出比输入少了 47 个 sample。**

**缺陷**：`AV_ROUND_NEAR_INF` 是"对半进位"（banker's rounding 的变体）。每次 rescale 都可能损失 <1 sample 的精度。连续累加 → 误差累计。14112000/44100 = 320，比率刚好是整数，但如果 pts 不是 320 的倍数，rescale 就丢了余数。

**影响**：输出时长比输入短 ~1ms（10000 帧 × 0.005 sample/frame 误差 ≈ 47）。对 A/V 同步或连续拼接致命。

**修复**：用 `av_rescale_q_rnd(pts, in_tb, out_tb, AV_ROUND_UP)` 或 `AV_ROUND_PASS_MINMAX`，或者干脆只 rescale 一次（第一个 frame 的 pts），后续用 `pts += frame->nb_samples` 自己累加。

**教训**：音频 pts 的"金标准"是累加样本数，不是每帧 rescale。rescale 用于跨容器时间基换算，不用于连续流。

---

**Q15. decoder 出来的 frame 直接送 encoder，没搬 `pts`。encoder 每次都输出 `pts=0` 或警告 "non-monotonic pts"。**

**缺陷**：`avcodec_receive_frame` 出来的 frame 带 `pts`（来自 `av_packet`），但如果你自己 alloc 了一个 AVFrame 去填数据，`pts == AV_NOPTS_VALUE`（=INT64_MIN）。encoder 的 `avcodec_send_frame` 看到 `AV_NOPTS_VALUE` 的行为依赖 encoder：
- libmp3lame：警告 + 自动累加 `frame_num * frame_size`
- libopus：直接当 0 用 → 全部帧 pts 一样 → muxer 警告 non-monotonic

**影响**：输出文件时间戳错乱，播放器看不到正确时长。

**修复**：
```c
out_frame->pts = av_rescale_q(in_frame->pts,
                              dec_ctx->pkt_timebase,
                              enc_ctx->time_base);
```

**教训**：frame pts 不会自动传递。转码链路上每一步你都要手动搬。或者用 `af_asetpts` filter 让 filtergraph 代劳。

---

**Q16. 用 `av_opt_set_int(dec_ctx, "some_opt", 1, 0)` 给 decoder 设了一个它不存在的 option。函数返回 0（成功），`avcodec_open2` 后行为异常。**

**缺陷**：`av_opt_set_int` 查找 option 的流程是 `AVCodecContext.av_class` → codec 的 `priv_class`。如果都找不到返回 `AVERROR_OPTION_NOT_FOUND` (`-0x54504ffff`)。**返回 0 说明它在某个地方找到了同名 option**——可能是 AVCodecContext 的通用 option（例如 `b`、`bit_rate`），不是你以为的 decoder private option。

**影响**：你以为设了 decoder 的 X，实际设了 AVCodecContext 的 `b`（bitrate）。decoder 初始化按 bit_rate = 1 算，完全错乱。

**修复**：
1. 始终检查 `av_opt_set_int` 返回值
2. 用 `av_opt_find(ctx, name, NULL, 0, AV_OPT_SEARCH_CHILDREN)` 先确认 option 存在于哪个 class
3. 给 private options 走 `AVDictionary *opts` + `avcodec_open2(ctx, codec, &opts)`，open 后检查剩余字典看哪些被拒绝

**教训**：`av_opt_set_*` 的 name 空间是全局的；decoder/encoder/muxer 的 private options 叫 `av_class->priv_class`。**检查返回值 + 用 AVDictionary 残留检测**。

---

**Q17. `avformat_open_input` 返回负值失败。用户清理时只调 `avformat_free_context(fmt)`。`valgrind` 报内部的 `pb` 泄漏。**

**缺陷**：`avformat_open_input` 失败时**会自己清理**传入的 `fmt_ctx`（释放 `pb` 等），并把 `*ps` 设成 NULL。用户以为它没清理，再调 `avformat_free_context(fmt)` 对 NULL 是 no-op——**没问题**。

**真正的 bug**：如果 open 部分成功（比如走到 `avformat_find_stream_info`）但后续失败，你要用 `avformat_close_input(&fmt_ctx)` 而不是 `avformat_free_context`。前者会释放 IO context、options、metadata、流数组；后者只释放结构体本身，**pb 会泄漏**。

**影响**：文件句柄 + 内存泄漏。

**修复**：
```c
AVFormatContext *fmt = NULL;
if (avformat_open_input(&fmt, url, NULL, NULL) < 0) return -1;
// ...
avformat_close_input(&fmt);  // 配对 open_input
```

**教训**：`avformat_free_context` 和 `avformat_close_input` 不一样。open 成功后的清理必须用 close。`alloc` 配 `free`，`open` 配 `close`。

---

**Q18. `avcodec_open2` 后改 `dec_ctx->ch_layout.nb_channels = 4`。下一次解码行为异常。**

**缺陷**：`avcodec_open2` 之后 decoder 内部已经**根据当时的 ch_layout 分配了 rematrix/downmix 矩阵、per-channel 状态**。用户改 `nb_channels` 只改了 AVCodecContext 里的字段，decoder 内部的 `priv_data` 没跟着变。

**影响**：per-channel 状态数组按 2 分配，你写入 channel 3 时越界；或者 decoder 继续按 2 解码，多出来的通道输出随机数据。

**修复**：通道数变化时必须 `avcodec_close + avcodec_open2` 重新初始化。更好的做法是监听 decoder 的 "parameters changed" 事件——FFmpeg 里的 `AV_CODEC_FLAG_OUTPUT_CORRUPT` + `send_packet` 返回 `AVERROR_INPUT_CHANGED` 通知用户重建 context。

**教训**：FFmpeg 的 context 不能"半热更新"。任何影响内部分配的字段变化都要重开。

---

**Q19. `swr_convert(ctx, out, N, in, M)` 返回 `converted == 0`，代码把这当错误处理。为什么是正常情况？**

**缺陷**：`swr_convert` 返回值是"本次吐出的样本数"。返回 0 的合法情况：
1. 输入样本还在延迟缓冲里累积，尚不够吐出一个 full output frame
2. 上采样初期：多相滤波器的 history 不够，吐不出
3. flush 最后一次调用（`in == NULL`）之后，已经没有残留

**影响**：把 0 当错误 → 退出循环 → 丢尾部几十毫秒。

**修复**：只有 **负返回值** 是错误。0 是"这次没吐，继续喂"。flush 循环：
```c
for (;;) {
    int got = swr_convert(swr, &out, out_count, NULL, 0);
    if (got < 0) { err(); break; }
    if (got == 0) break;  // 真的排空了
    write(out, got);
}
```

**教训**：FFmpeg 里区分"错误"和"暂无输出"是通用模式。负 = 错，0 = 暂无，正 = 成功。EAGAIN 只出现在 `send/receive` 系列。

---

**Q20. `avcodec_send_packet` 返回 `AVERROR(EAGAIN)`。代码直接 return -1 退出。**

**缺陷**：`EAGAIN` 在 send 的语义是"我内部 output buffer 满了，你先 receive 一帧腾地方再 send"。不是错误。API 契约见 `avcodec_send_packet` 文档：
> AVERROR(EAGAIN): input is not accepted in the current state - user must read output with avcodec_receive_frame()

**影响**：一遇到 decoder 产出多帧（1 packet → 2+ frame，SBR/PS AAC 典型）就退出，后面所有数据丢失。

**修复**：
```c
while ((ret = avcodec_send_packet(dec, pkt)) == AVERROR(EAGAIN)) {
    if ((ret = avcodec_receive_frame(dec, frame)) < 0) break;
    handle(frame);
    av_frame_unref(frame);
}
```
或者更对称地：主循环先 drain 所有 receive，再 send 下一个 packet。

**教训**：send 的 EAGAIN 和 receive 的 EAGAIN 方向相反。同一个错误码在两个函数里含义对立——这是 FFmpeg push 式 API 的最大坑点。**只把负返回值中的 "非 EAGAIN/非 EOF" 当错误**。

---

### 3K. FFmpeg 源码真实 bug（音频）（Q21-Q40）

以下 bug 均来自 FFmpeg 上游 git（`554dcc2885` 快照可验证）。每条 commit 都能 `git show <sha>` 还原。

---

**Q21. FFmpeg commit `11a5afea31` 修了一个 DCA-XLL 无损子流的 bug，`get_rice_array` 在 bitstream 不足时继续读，导致后续解码使用未初始化的 `int32_t` 数组。根本原因是什么？**

**缺陷**：`libavcodec/dca_xll.c:67` 的 `get_rice_array` 以前签名是 `void` 返回，循环只看 `i < size`，**不检查 `get_bits_left(gb)`**。当流异常短、Rice 码的 unary 前缀读光 bit 时，`get_rice` 返回 0 并继续填 `array[i]`——实际上填的是"刚才越过 bitstream 末尾后的默认值"，留下**未初始化的语义**。

**影响**：OSS-Fuzz #451655450 报告。后续 `chs_parse_band_data` 用 `part_a[]`、`part_b[]` 做 LPC 预测重构 → 根据未初始化内存做运算 → MSAN/UBSan 报 use-of-uninitialized-value；实际输出是垃圾 PCM 但不 crash，安全场景下可能泄漏栈数据。

**修复**（`libavcodec/dca_xll.c:67-76`）：
```c
static int get_rice_array(GetBitContext *gb, int32_t *array, int size, int k)
{
    for (i = 0; i < size && get_bits_left(gb) > k; i++)
        array[i] = get_rice(gb, k);
    if (i < size)
        return AVERROR_INVALIDDATA;
    return 0;
}
```
调用点（`libavcodec/dca_xll.c:536, 569`）传播错误。

**教训**：bitstream 解析**每一层都要检查 `get_bits_left`**。不检查等于"填入未初始化的语义 0"，所有 UBSan 检不出，但是真实的语义空洞。

---

**Q22. FFmpeg commit `af86f0ffcc` 给 DCA-XLL 的 PBR buffer 末尾加 `memset 0`。修的是什么？**

**缺陷**：`ff_dca_xll_parse` 把压缩数据拷到内部 `pbr_buffer`，后续 `GetBitContext` 按 size 初始化读取。但 FFmpeg 的 bitstream reader 为了性能会**多读几字节**（一次 32/64 bit load）——要求 buffer 末尾有 `AV_INPUT_BUFFER_PADDING_SIZE`（=64）字节 padding。如果 pbr_buffer 末尾残留上次残留数据，读 padding 就是读旧内容。

**影响**：MSAN 抱怨 use-of-uninitialized value；功能上解码结果不确定——取决于上次 pbr_buffer 里的垃圾。

**修复**（`libavcodec/dca_xll.c:1103, 1160`）：
```c
memset(s->pbr_buffer + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
```

**教训**：FFmpeg bitstream reader 的 padding 约定是**隐含前提**，每个把数据拷进 buffer 再当 bitstream 读的地方都必须 memset padding。新 demuxer/parser 容易踩。

---

**Q23. FFmpeg commit `60f49f4d92` 把 QDM2 decoder 初始化里一堆 `bytestream2_*` 调用换成 `_*u` 版本，并把 "bytes_left < 12" 改成 "< 44"。修了什么？**

**缺陷**：`qdm2_decode_init` 读 QDCA extradata 时分多次调 `bytestream2_get_be32`（带边界检查）。每次都检查有"剩余字节吗" → 正常路径每次都走一遍 check，浪费；但更严重的是**老代码 `bytes_left < 12` 写错了**——真实需要读 12+ 个字段约 44 字节，不够时后续 `get_be32` 逐个触发**隐式**边界检查——每个返回 0，导致 `nb_channels = 0`、`sample_rate = 0` 等，decoder 内部数组按 0 长分配，后续访问 OOB。

**影响**：损坏的 QDCA 流可以让 decoder 用全 0 配置初始化，分配 0 字节数组，解码 frame 时 buffer 读写越界。

**修复**（`libavcodec/qdm2.c:1731-1717`）：先一次性检查 `bytes_left >= 44`，然后全走 `_u` 版本（unchecked）跳过冗余检查，同时明确"这段数据量够"：
```c
if (bytestream2_get_bytes_left(&gb) < 44) { ret = AVERROR_INVALIDDATA; goto fail; }
bytestream2_skipu(&gb, 8);
size = bytestream2_get_be32u(&gb);
```

**教训**：parser 开头**一次性检查最大读取量**，后续用 unchecked 版本。既快又把边界条件集中到一处，易审计。

---

**Q24. FFmpeg commit `f6986e75be` 修了 Speex decoder 的一个 OOB：`sb_decode` 递归调用 `nb_decode` 或另一个 `sb_decode`，fuzzer 构造嵌套能让它写入越界数组。根本原因是什么？**

**缺陷**：Speex 的 wideband codec 用**嵌套 submode**：一个 sb_decode 里可以有 nb_decode 或更深的 sb_decode。decode 函数内部把样本写入 `out[i * frame_size]`，`i` 由 `frames_per_packet` 控制。原代码的递归不传"还剩多少 packet 可用"给下一层，下一层以为自己是顶层可以填 full frame → 写越界。

**影响**：OSS-Fuzz #394638693 报告 out-of-array access。

**修复**（`libavcodec/speexdec.c:172, 524, 870, 1221, 1234, 1561`）：给 decode 回调加 `int packets_left` 参数，递归时 `packets_left - 1` 传下去：
```c
if (packets_left <= 1) return AVERROR_INVALIDDATA;
ret = speex_modes[st->modeID - 1].decode(avctx, &s->st[...], gb, out, packets_left);
```
顶层 `speex_decode_frame` 传 `frames_per_packet - i`。

**教训**：**递归代码要显式传递剩余预算**。隐式假设"调用者保证足够"在 decoder 里永远是 bug——bitstream 可以让调用约定凭空成立。

---

**Q25. FFmpeg commit `5b112b17c0` 给 opus CELT decoder 加了一行 `if (!isnormal(block->emph_coeff)) block->emph_coeff = 0.0;`。为什么？**

**缺陷**：CELT 的 post-filter 用 `emph_coeff` 做 1-tap IIR（去加重）。静音帧输入全 0 → 输出逐步衰减 → 某一帧后 `emph_coeff` 进入**次正规浮点数**（denormal/subnormal）范围（< `2^-126`）。x86 上对 denormal 的浮点运算比 normal 慢 10-100 倍（微码陷入）。

**影响**：scrcpy 用 opus 传输屏幕录制的静音段落时 CPU 飙升。参见 scrcpy issue #6715。

**修复**（`libavcodec/opus/dec_celt.c:466-467`）：
```c
if (!isnormal(block->emph_coeff))
    block->emph_coeff = 0.0;
```

**教训**：**信号处理的 IIR 反馈路径要守卫 denormal**。Intel/AMD 对 denormal 走 microcode 异常路径，性能崩。实践上：反馈路径加小偏置（DC offset）、用 `_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON)`、或显式 clamp。这种 bug 不是 correctness bug 是**性能断崖**。

---

**Q26. FFmpeg commit `7086491fa0` 把 mpegaudio handle_crc 里一个移位操作改成 unsigned。修了什么 UB？**

**缺陷**（`libavcodec/mpegaudiodec_template.c:382`）：
```c
((buf[6 + sec_byte_len] & (0xFF00 >> sec_rem_bits)) << 24) + ...
```
`0xFF00 >> sec_rem_bits` 是 int 运算，可能算出 192（`0xC0`）。`192 << 24` = `0xC000_0000`，超出 `int` 范围（`INT_MAX == 0x7FFF_FFFF`）→ **有符号左移溢出 = UB**。

**影响**：OSS-Fuzz #49577 报告。`-fsanitize=shift` 触发。实际运行里 GCC/Clang 多数翻译成按位运算没问题，但 UB 就是 UB，优化器可以做任何事。

**修复**：把 mask 改成 `0xFF00U`（unsigned）：
```c
((buf[6 + sec_byte_len] & (0xFF00U >> sec_rem_bits)) << 24) + ...
```
unsigned 左移溢出是 well-defined（模 `2^32` 环绕）。

**教训**：C 里"1 字节 OR 掩码 << 24"是移位 UB 高发区。`0xFF << 24` 是 UB，`0xFFU << 24` 不是。FFmpeg 大量这类 fix 集中在 mp3、flac、aacsbr。

---

**Q27. FFmpeg commit `56b85b689d` 修了 AAC USAC 算术编码上下文：`state->cur[0] = a + b + 1; if (state->cur[0] > 0xF) state->cur[0] = 0xF;`。这里 bug 在哪？**

**缺陷**（`libavcodec/aac/aacdec_ac.c:94`）：`state->cur[0]` 是 **`uint8_t`**。`a + b + 1` 是 int。如果 `a + b + 1 > 255`，赋值到 uint8_t 时**先截断到 8 bit**（value & 0xFF），**然后才和 0xF 比**。举例 `a=200, b=60`：`a+b+1 = 261`，截断 → `5`，`5 > 0xF` 为假，结果保存为 5，而不是 clamp 到 15。

**影响**：USAC 算术解码器上下文状态错乱。commit message 原话"solves numerous bitstream desyncs, particularly when coefficients with magnitude greater than 127 are sent"——解码大系数的高质量 USAC 音频时内容混乱。

**修复**：
```c
state->cur[0] = FFMIN(a + b + 1, 0xF);
```
`FFMIN` 是宏展开后仍是 int 运算，先比较再赋值。

**教训**：**"先赋值再 clamp" 在窄类型上永远是 bug**。正确顺序：先在宽类型里 clamp，再赋值。这是 lossy encoder/decoder 里经典坑。

---

**Q28. FFmpeg commit `2f59648aed` 修了 WavPack decoder 在 `av_realloc_f` 失败时的 crash：双指针数组被释放后 `fdec_num` 没更新。**

**缺陷**（`libavcodec/wavpack.c:975`）：`av_realloc_f` 在**失败时 free 原 buffer 并返回 NULL**。旧代码：
```c
c->fdec = av_realloc_f(c->fdec, c->fdec_num + 1, sizeof(*c->fdec));
if (!c->fdec) return AVERROR(ENOMEM);
```
问题：`c->fdec` 是**指向所有权指针数组**的指针，每个元素都是 decoder 子上下文的 owning ptr。`av_realloc_f` 把整个外层数组 free 了——**里面的 owning ptr 全部泄漏**。而且 `fdec_num` 没更新，decoder 的 `close` 函数用 `for i < fdec_num: free(fdec[i])` 就走到 NULL→free → NPD。

**影响**：OOM 场景下泄漏 + NPD。回归源于 46412a8935，之前的代码没这个问题。

**修复**（`libavcodec/wavpack.c:975-978`）：改用 `av_realloc_array`（失败不 free 原 buf），手动判断：
```c
WavpackFrameContext **fdec = av_realloc_array(c->fdec, c->fdec_num + 1, sizeof(*c->fdec));
if (!fdec) return AVERROR(ENOMEM);
c->fdec = fdec;
```

**教训**：`av_realloc_f` 的"失败时 free 原 buf"语义是 FFmpeg 独有的反直觉行为。**持有所有权指针的数组永远不要用 `realloc_f`**——要么用 `realloc_array`（失败不 free），要么用 `av_reallocp`（保持原指针完整）。

---

**Q29. FFmpeg commit `515c0247a3` 修了 APE 解码器 `predictor_update_filter` 一行的有符号整数溢出。fuzzer 喂进特定输入就能触发。**

**缺陷**（`libavcodec/apedec.c:1220`）：
```c
p->lastA[filter] = decoded + ((int32_t)(predictionA + (predictionB >> 1)) >> 10);
```
两个 32-bit 中间量相加可能溢出 `int32_t`。fuzzer 示例：`-2147483506 + -801380 = -2148285886`，不在 int 范围内 → **signed overflow = UB**。

**影响**：OSS-Fuzz #62164 报告。UBSan 立即触发。`-O2` 下编译器可能按"signed overflow 不发生"做假设优化，产生任意行为。

**修复**：把累加强转到 unsigned，最后转回：
```c
p->lastA[filter] = (int32_t)(decoded + (unsigned)((int32_t)(predictionA + (predictionB >> 1)) >> 10));
```
`(unsigned)` 让加法在 uint32_t 域做，well-defined 模 `2^32` 环绕；外层 `(int32_t)` 是 implementation-defined 但 x86/ARM 都按位拷贝。

**教训**：APE decoder 的 predictor 是"累加 + 移位"的 IIR 滤波器，**每一个累加点都是 signed overflow 候选**。FFmpeg 里 apedec.c 同期修了 `2def617787`、`1887ff250c` 三个类似的 —— 系统性问题。

---

**Q30. FFmpeg commit `2def617787` 把 APE decoder 里 `int32_t right = left + a0;` 改成 `int32_t right = left + (unsigned)a0;`。为什么一个 cast 就修复？**

**缺陷**（`libavcodec/apedec.c:1289`）：`left` 和 `a0` 都是 `int32_t`。`left + a0` 是 signed int 加法，溢出是 UB。fuzzer 例子：`1900031961 + 553590817 = 2453622778 > INT32_MAX`。

**影响**：OSS-Fuzz #63061 签名整数溢出 UB。

**修复**：一个操作数转 `unsigned` → 整个表达式变成 unsigned 加法（C 的 usual arithmetic conversion）→ 模 `2^32` 环绕 well-defined → 转回 `int32_t` 取模 2^32 的位模式。

**教训**：C 里 `(int) + (unsigned)` 的结果是 unsigned，这是 well-defined 的"无损"补码环绕。FFmpeg、libopus、libvpx 全都用这个"加 unsigned cast" 的技巧来修 signed overflow UB，**不需要真正改用 `int64_t`**。

---

**Q31. FFmpeg commit `1887ff250c` 把 APE decoder 的 clamp 条件 `FFMAX(FFABS(left), FFABS(right)) > (1<<23)` 改成 `FFMIN(FFNABS(left), FFNABS(right)) < -(1<<23)`。为什么要用 NABS？**

**缺陷**（`libavcodec/apedec.c:1287`）：`FFABS(INT32_MIN)` 是 **UB**——`-INT32_MIN` 不能用 `int32_t` 表示（补码里 `INT32_MIN = -2147483648`，正数最大 `2147483647`）。fuzzer 输入让 `left == INT32_MIN` → `FFABS(left)` = UB negation。

**影响**：OSS-Fuzz #67738 报告 "negation of -2147483648 cannot be represented in type 'int32_t'"。

**修复**：用 `FFNABS(x) = ((x) > 0 ? -(x) : (x))`——**绝对值的负数**。`FFNABS(INT32_MIN) = INT32_MIN`（well-defined：正数取负不会溢出，负数不变）。然后比 `< -(1<<23)` 等价于原来的 `FFABS > (1<<23)`。

**教训**：**`abs` 在补码机器上对 `INT_MIN` 是 UB**。安全替代：NABS（非负→取负），或者先强转 unsigned 再位运算。任何用户输入驱动的绝对值都要换成 NABS 或 `llabs((int64_t)x)`。

---

**Q32. FFmpeg commit `08383443ff` 把 Vorbis decoder 的 `vorbisfloat2float` 里 `double mant / long exp` 改成 `float mant / int exp`，并用 `ldexpf` 替代 `ldexp`。这是性能优化还是 bug 修复？**

**缺陷**（`libavcodec/vorbisdec.c:186-188`）：Vorbis codebook 的最小值是用 "21-bit mantissa + 10-bit exponent + sign" 自定义浮点编码的。老代码用 `double` + `ldexp`，结果再赋回 `float`——**double → float 的舍入方向依赖 FPU 舍入模式**。极少数情况下 double 里的 mid-range 值舍入到 float 后变成和其它 codebook 项相同的 float 值 → codebook 碰撞 → 解码歧义。

**影响**：commit message 没明确说 bug，但 James Almer 的修改显然是为了让 "21-bit mantissa" 直接走 float 运算（float 24-bit 尾数足够表达 21-bit），避免 double 中间态的舍入歧义。同时 `ldexpf` 比 `ldexp` 快。

**修复**：
```c
float mant = val & 0x1fffff;
int   exp  = (val & 0x7fe00000) >> 21;
return ldexpf(mant, exp - 20 - 768);
```

**教训**：**float → double → float 的往返不是恒等变换**（FP 运算 round-to-nearest-even + 中间精度）。算法明确要求"float 精度"时不要用 double 中间量。音频 DSP 尤其注意。

---

**Q33. FFmpeg commit `dc89cf804a` 给 Vorbis residue decode 加了一个 `if (get_bits_left(gb) <= 0) return AVERROR_INVALIDDATA;` 检查。没这个检查会死循环——为什么？**

**缺陷**（`libavcodec/vorbisdec.c:1468`）：`vorbis_residue_decode_internal` 的核心循环按 codebook 读变长码。`get_vlc2` 在流耗尽时会返回 "fallback" 值（通常是 0）而不是 error。如果 fallback 值让循环**不前进**（比如 step 变 0），循环变死循环。

**影响**：OSS-Fuzz #66326 报告 timeout。拒绝服务 attack vector。

**修复**：循环里检查 `get_bits_left(gb) <= 0` 主动退出。

**教训**：**解码循环里除了正常终止条件，还要兜底"bitstream 已耗尽" 条件**。`get_vlc2` 不报错只返 fallback 的 API 契约容易让循环卡死。

---

**Q34. FFmpeg commit `cadd7e7a75` 给 Vorbis codebook 解析加了 `isfinite` 检查：`codebook_minimum_value`、`codebook_delta_value`。为什么是 timeout 而不是 crash？**

**缺陷**（`libavcodec/vorbisdec.c:372`）：Vorbis setup header 里 codebook 包含浮点最小值和增量。如果 attacker 构造 `NaN` 或 `Inf`，后续 codebook 表的生成循环用它做加法：`entry = min + delta * i`。`NaN + anything = NaN`，不会爆 → 所有 entries 全是 NaN → 查表返回 NaN → 信号处理 NaN 传播，某些分支比较 NaN 永远为假 → **循环永不终止**。

**影响**：OSS-Fuzz #55116 报告 timeout / DoS。

**修复**：
```c
if (!isfinite(codebook_minimum_value) || !isfinite(codebook_delta_value)) {
    ret = AVERROR_INVALIDDATA;
    goto error;
}
```

**教训**：**浮点 parser 要守卫 NaN/Inf**。`NaN` 的"毒性传播" 可以让 decoder 陷入死循环而非 crash，这是最阴险的 DoS：程序活着但卡住。

---

**Q35. FFmpeg commit `ae81beb351` 给 AAC decoder close 函数加了一行 `av_channel_layout_uninit(&ac->oc[i].ch_layout)`。为什么这行是泄漏？**

**缺陷**（`libavcodec/aac/aacdec.c:1144`）：AAC decoder 的 OutputConfiguration 数组 `ac->oc[]` 每个成员都有一个 `AVChannelLayout`。解码过程中遇到布局变化时会 `av_channel_layout_copy(&oc->ch_layout, ...)`——如果源是 **CUSTOM** layout（非 NATIVE），`copy` 会 `av_malloc_array` 新的 map。

decoder 的 close 函数以前没 uninit 这些 ch_layout → CUSTOM map 数组泄漏。

**影响**：正常流多数是 NATIVE，`uninit` 对 NATIVE 是 no-op，所以大部分用户看不到泄漏。但 20+ 声道的 DVD-Audio / Ambisonic AAC 使用 CUSTOM 布局时，每次 close 泄漏 `nb_channels * sizeof(AVChannelCustom)`。长跑的 transcoding server 累计成 MB 级泄漏。

**修复**：
```c
for (i = 0; i < MAX_ELEM_ID; i++)
    av_channel_layout_uninit(&ac->oc[i].ch_layout);
```

**教训**：**`AVChannelLayout` 成员的生命周期管理是新 API 的重灾区**。凡是 `av_channel_layout_copy` 过的都要 `uninit`。NATIVE 情况下 uninit 是 no-op 不影响正确性，但 CUSTOM 必须释放。防御性编程："只要有 copy，就一定有 uninit"。

---

**Q36. FFmpeg commit `3f2b452a22` 修了 opus SILK decoder 的 `silk_decode_superframe`：原代码把 `active[1][i]` 传给 `silk_decode_frame`，但 mono 输入时 `active[1]` 未初始化。**

**缺陷**（`libavcodec/opus/silk.c:844-848`）：`active[2][SILK_MAX_FRAMES]` 在 superframe 开头按 `coded_channels` 初始化。mono 时只填 `active[0][]`，`active[1][]` 是栈垃圾。循环里：
```c
for (j = 0; j < coded_channels && !s->midonly; j++)
    silk_decode_frame(s, rc, i, j, coded_channels, active[j][i], active[1][i], 0);
                                                                 ^^^^^^^^^^^^^
                                                                 mono 下未初始化
```

**影响**：MSAN 报 use-of-uninitialized-value。运行时表现为 mono opus 偶发爆音（依赖栈垃圾值）。

**修复**：mono 时传 0：
```c
int active1 = coded_channels > 1 ? active[1][i] : 0;
silk_decode_frame(s, rc, i, j, coded_channels, active[j][i], active1, 0);
```

**教训**：**stereo / mono 分支里，stereo 专用字段在 mono 下可能未初始化**。不能假设"mono 时第二声道的字段会被忽略"——只要你把它传进函数，函数就可能读它。

---

**Q37. FFmpeg commit `464fb861b1` 把 AAC USAC LPD 里 `get_unary(gb, 0, INT32_MAX)` 改成 `get_unary(gb, 0, 68)`。为什么 INT32_MAX 是 bug？**

**缺陷**（`libavcodec/aac/aacdec_lpd.c:65, 78, 88`）：`get_unary(gb, stop_bit, max)` 读 unary code，最多读 `max` bit。原代码传 `INT32_MAX` = "无限制"，配合不信任的输入，一个全 0 bitstream 让它读 `INT32_MAX` 个 0。后续代码把返回值存到 32 bit 字段，产生溢出：
> signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'

**影响**：OSS-Fuzz #393164866 报告。AAC LATM streams 可触发。UBSan 抓到，功能上 decoder 陷入死循环或写越界。

**修复**：按"后续存储容量"限制上界：
```c
qn[k] = get_unary(gb, 0, 68);  // 后续存 32bit，68 是合规上限
```
以及加一个全局的 `if (nk > 25) return AVERROR_PATCHWELCOME;` 边界（`libavcodec/aac/aacdec_lpd.c:81`）。

**教训**：**bitstream parser 的每个 `get_*` 都要传现实上界**。INT32_MAX/SIZE_MAX/-1 的"不限制" 等于把攻击者当合作者。

---

**Q38. FFmpeg commit `01a1b99fc2` 给 aacsbr_template.c 加了 `if (ilb >= 40) return;`。为什么这个边界需要在运行时检查？编译器不能静态证明吗？**

**缺陷**（`libavcodec/aacsbr_template.c:1634, 1655`）：SBR 解码里 `f_master[ilb][...]` 访问一个 `INTFLOAT [40][2]` 数组。`ilb` 来自 bitstream。原代码没检查上界，fuzzer 构造让 `ilb = 50` → OOB。

**影响**：OSS-Fuzz #401661737 报告 "index 50 out of bounds for type 'INTFLOAT [40][2]'"。实际写入越界栈/堆内存。

**修复**：直接 `return`（bailout，不做这个 SBR 处理）：
```c
if (ilb >= 40)
    return;
```
commit message 明说 "Someone knowing AAC well should review this, there is likely a nicer fix"——典型的"先修安全再优化" 应急补丁。

**教训**：编译器无法静态证明 bitstream-derived 索引的范围——它只看到 `get_bits(gb, N)`。开发者必须在用索引前做 runtime clamp。SBR 表尺寸硬编码（40、64 等）是 codec 规范，**所有 bitstream-derived 的索引访问这些表前都要 bounds check**。

---

**Q39. FFmpeg commit `3f029bfb7f` 给 aacsbr_template.c 加了一行 `sbr->n_q = 1` 在错误路径上。为什么不重置会 OOB？**

**缺陷**（`libavcodec/aacsbr_template.c:594, 602`）：SBR `n_q` 是噪声楼层分段数，后续循环按 `i < sbr->n_q` 访问 `arrays[i]`（size 5）。某个 error path 上 `n_q` 没重置，保留上次解码时的大值（比如 7），触发 "index 5 out of bounds for type 'uint8_t [5]'"。

**影响**：OSS-Fuzz #377748135 报告。AAC LATM fuzzer 构造先产生大 n_q，再触发 error path。

**修复**：错误路径 `sbr->n_q = 1;`（最小合法值）。

**教训**：**decoder 的状态机错误路径必须把状态复位到"下一帧可以安全继续"的最小状态**。留下上次的值是典型的错误恢复 bug，正常测试路径覆盖不到。与 QDM2 的 `sub_packet` UAF（commit `a795ca89fa`）是同一类模式。

---

**Q40. FFmpeg commit `112a077d06` 在 FLAC 33-bit LPC decoder 里把 `(int64_t)residual[i] + (int64_t)decoded[i-1]` 改成 `(uint64_t)...`。与我们已经讨论的 33-bit LPC overflow（`fd7352660b`，Source Bug #3）是同一个还是不同的？**

**缺陷**（`libavcodec/flacdec.c:368-380`）：这是 FLAC 的 `DECODER_SUBFRAME_FIXED_WIDE` 宏，处理 33-bit-per-sample 的 fixed predictor（阶数 1-4），**不是** LPC（`fd7352660b` 修的是 `DECODER_SUBFRAME_LPC`）。fuzzer 构造让 residual + decoded 的累加超过 `INT64_MAX`，signed overflow UB。

**影响**：和 #3 是同一个"模式"（FLAC 33-bit 累加 UB），但在**不同的函数**。commit message 明说 "Fix integer overflow in '33bit' DECODER_SUBFRAME_FIXED_WIDE"。

**修复**：把所有操作数转 `uint64_t`，让加法和乘法都在 unsigned 域做：
```c
decoded[i] = (uint64_t)residual[i] + 2*(uint64_t)decoded[i-1] - (uint64_t)decoded[i-2];
```

**教训**：**同一个 codec 的相似代码路径往往有相似 bug**。修了 LPC 的 overflow 后一定要审计同文件里 fixed predictor、decorrelate、rematrix 等所有累加点。FFmpeg FLAC decoder 在 2024 年同一时期连修了 `fd7352660b`、`112a077d06`、`35e6960a6b` 三个同类 overflow—— **系统性问题需要系统性审计**，不是一次 hotfix。

---

**Q41. swresample/resample: `filter_length=1 && phase_count=1` 时定点分数向下取整导致重采样可听见失真（commit `7b1b9bb31f`，作者 Marton Balint，2024-02-27）**

**Situation**
`libswresample/resample.c:357` 的 `multiple_resample()` 为"退化情形"（`filter_length == 1 && phase_count == 1`，等价于最近邻无插值 + 没有多相分解）写了一条 fast path：用 Q32 定点数累加相位，直接 `dst[i] = src[index2 >> 32]` 跳采样。核心两行：
```c
int64_t index2 = (1LL<<32)*c->frac/c->src_incr + (1LL<<32)*c->index;
int64_t incr   = (1LL<<32) * c->dst_incr / c->src_incr;
```
其中 `c->frac / c->src_incr` 是输入流内当前整数 sample 的分数偏移，`c->dst_incr / c->src_incr` 是每输出一个目标 sample 要推进的输入位置增量。

**Task**
用户在邮件列表报告 `aresample=24000:filter_size=1:phase_shift=0` 做 8000→24000 Hz 上采样时，依输入 chunk 大小不同，输出有**周期性咔嗒声**。最小复现：
```bash
ffplay -f lavfi -i "sine=440:r=8000:samples_per_frame=32,aresample=24000:filter_size=1:phase_shift=0"
```
440Hz 正弦波本该是纯音，实际听到规律性失真——说明 index 在跨 chunk 边界时出现了错位。

**Action**
根因是两次整数除法都**向下截断**（C 语言 `int64_t` 除法行为）：
- 理想 `incr = 8000 / 24000 ≈ 0.333...` → `(1<<32) * 1 / 3 = 1431655765`（少了 0.333... 的无穷小尾数）
- 每累加一次 `index2 += incr`，真实相位比定点值多了一点点；累计 N 次后，定点 `index2 >> 32` 会**比真实输入索引少 1**，此时同一个输入 sample 被取两次（或跨 chunk 时上一轮已经 `c->index` 推进过，结果两边接不上）——听感就是每 `src_incr / gcd` 个样本出一次错。

误差方向固定（永远偏小），不会自己回正。Marton 的修复是让两个分数都**向上取整**（加 1 ULP）：
```diff
- int64_t index2 = (1LL<<32)*c->frac/c->src_incr + (1LL<<32)*c->index;
- int64_t incr   = (1LL<<32) * c->dst_incr / c->src_incr;
+ int64_t index2 = (1LL<<32)*c->frac/c->src_incr + (1LL<<32)*c->index + 1;
+ int64_t incr   = (1LL<<32) * c->dst_incr / c->src_incr + 1;
```
为什么 "+1" 就对？因为 `incr` 永远偏大 **最多 1 ULP**（`2^-32 × src_incr`），累加 `dst_size` 次最多让 `index2` 多推进 `dst_size` ULP，远不足以让 `>> 32` 结果跳过一个样本（需要推进整整 `2^32` 才跳一个）。但累加的"永远偏小"会在有限步内真的让 `>>32` 少 1——对称被打破。向上舍入把方向翻转为"偏大但不足以造成跨样本错位"，恰好吃掉了原 bug。配套修改 `tests/fate/libswresample.mak` 的黄金参考值。

**Result**
Cherry-pick 到 6.1 / 5.1 / 4.4 稳定分支（对应 `7b1b9bb31f` / `ef327c189f` / `c18115b413` / `3fb9425a75`）。后续没有再出现相关报告。

**学到的东西**
1. **定点数除法的舍入方向不是中立的**——当误差不会被反向操作抵消时，"向下取整"会单调累积，"向上取整"同样单调累积，但**选错方向就是 bug**。遇到"分数累加 + 整数抽取"的定点 DSP 代码，一定要推演误差方向。
2. **fast path 是 bug 温床**：正常路径走多相 FIR（`filter_length > 1`），误差被滤波器自身的低通平均掉；fast path 退化成纯最近邻，任何相位错位都直接可听。测试必须覆盖退化参数。
3. **可听 bug 的定位方法**：纯正弦波 + 极端参数 + 小 chunk，周期性失真立刻暴露——比随机音乐信号好用一万倍。代码里 `filter_size=1:phase_shift=0` 这种"极简参数"不是用户真会用的，但作为回归测试的边界条件无可替代。
4. 看到分数累加式 `(1LL<<32)*a/b + (1LL<<32)*c` 这种形式，先问两个问题：除法截断方向对不对？累加 N 次后的单调误差会不会跨越下一个整数边界？

---

### 3K 附：commit 索引速查

| # | SHA (short) | Codec/Library | 类别 |
|---|---|---|---|
| Q21 | `11a5afea31` | DCA-XLL | Rice decode uninit |
| Q22 | `af86f0ffcc` | DCA-XLL | bitstream padding memset |
| Q23 | `60f49f4d92` | QDM2 | extradata bytes_left check |
| Q24 | `f6986e75be` | Speex | 嵌套递归 packets_left |
| Q25 | `5b112b17c0` | Opus CELT | denormal 性能崩 |
| Q26 | `7086491fa0` | MP1/MP2/MP3 | shift UB (0xFF00 → 0xFF00U) |
| Q27 | `56b85b689d` | AAC USAC AC | uint8_t 先截断后 clamp |
| Q28 | `2f59648aed` | WavPack | av_realloc_f 失败泄漏 + NPD |
| Q29 | `515c0247a3` | APE | predictor_update_filter UB |
| Q30 | `2def617787` | APE | stereo 3950 signed overflow |
| Q31 | `1887ff250c` | APE | FFABS(INT_MIN) UB → FFNABS |
| Q32 | `08383443ff` | Vorbis | double→float 舍入歧义 |
| Q33 | `dc89cf804a` | Vorbis | residue 死循环 timeout |
| Q34 | `cadd7e7a75` | Vorbis | codebook NaN 毒性传播 |
| Q35 | `ae81beb351` | AAC | ch_layout CUSTOM 泄漏 |
| Q36 | `3f2b452a22` | Opus SILK | mono 下 stereo 字段未初始化 |
| Q37 | `464fb861b1` | AAC USAC LPD | get_unary INT32_MAX |
| Q38 | `01a1b99fc2` | AAC SBR | ilb OOB 40 |
| Q39 | `3f029bfb7f` | AAC SBR | 错误路径 n_q 未复位 |
| Q40 | `112a077d06` | FLAC 33-bit fixed | 同类 overflow 系统性 |
| Q41 | `7b1b9bb31f` | swresample resample | 定点分数向下取整导致可听失真 |
