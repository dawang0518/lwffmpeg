# audio_flow

网络 MP3 播放示例（FFmpeg + SDL2），`video_flow` 的音频版。

## 构建

```bash
cmake -B build && cmake --build build
```

依赖：本地编译的 FFmpeg 在 `../../ffmpeg-build/`（见项目根 `study/学习路线.md`），系统装了 SDL2（`brew install sdl2`）。

## 运行

```bash
./build/audio_flow                           # 默认 SoundHelix 测试 MP3
./build/audio_flow path/to/local.mp3         # 本地文件
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
