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
    if (fmt_ctx) avformat_close_input(&fmt_ctx);
    avformat_network_deinit();
    return exit_code;
}
