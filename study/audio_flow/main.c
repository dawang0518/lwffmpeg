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
