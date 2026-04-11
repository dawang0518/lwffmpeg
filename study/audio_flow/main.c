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
    AVCodecContext *dec_ctx = NULL;
    int audio_stream_idx = -1;

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

    exit_code = 0;

cleanup:
    if (dec_ctx) avcodec_free_context(&dec_ctx);
    if (fmt_ctx) avformat_close_input(&fmt_ctx);
    avformat_network_deinit();
    return exit_code;
}
