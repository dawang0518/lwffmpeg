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

/* SDL 输出目标格式（固定） */
#define OUT_SAMPLE_RATE 44100
#define OUT_SAMPLE_FMT  AV_SAMPLE_FMT_S16
#define OUT_CHANNELS    2

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
    SwrContext *swr_ctx = NULL;
    SDL_AudioDeviceID audio_dev = 0;
    int sdl_inited = 0;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;

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

    exit_code = 0;

cleanup:
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    if (sdl_inited) SDL_Quit();
    swr_free(&swr_ctx);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    avformat_network_deinit();
    return exit_code;
}
