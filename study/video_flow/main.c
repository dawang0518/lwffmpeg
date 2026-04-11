/**
 * 视频处理完整流程演示
 *
 * 流程: 解封装(demux) → 解码(decode) → 保存YUV帧
 *
 * 核心API调用顺序:
 *   avformat_open_input()          打开输入文件
 *   avformat_find_stream_info()    探测流信息
 *   avcodec_find_decoder()         找到解码器
 *   avcodec_alloc_context3()       分配解码器上下文
 *   avcodec_parameters_to_context() 拷贝流参数到解码器
 *   avcodec_open2()                打开解码器
 *   av_read_frame()                读取压缩包(AVPacket)
 *   avcodec_send_packet()          送入解码器
 *   avcodec_receive_frame()        取出原始帧(AVFrame)
 */

#include <stdio.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>

static void save_frame_as_pgm(AVFrame *frame, int width, int height, int frame_index)
{
    char filename[64];
    snprintf(filename, sizeof(filename), "frame_%04d.pgm", frame_index);

    FILE *f = fopen(filename, "wb");
    if (!f) return;

    // PGM 格式: 灰度图，直接写 Y 分量
    fprintf(f, "P5\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; y++) {
        fwrite(frame->data[0] + y * frame->linesize[0], 1, width, f);
    }
    fclose(f);
    printf("  -> 已保存 %s\n", filename);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <输入视频文件> [保存帧数]\n", argv[0]);
        fprintf(stderr, "示例: %s input.mp4 5\n", argv[0]);
        return 1;
    }

    const char *input_file = argv[1];
    int max_frames = 5;
    if (argc >= 3) {
        max_frames = atoi(argv[2]);
    }

    int ret;

    // ========== 第1步: 解封装 - 打开输入文件 ==========
    printf("=== 第1步: 打开输入文件 ===\n");

    AVFormatContext *fmt_ctx = NULL;
    ret = avformat_open_input(&fmt_ctx, input_file, NULL, NULL);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "无法打开文件 %s: %s\n", input_file, errbuf);
        return 1;
    }
    printf("  格式: %s\n", fmt_ctx->iformat->name);

    // ========== 第2步: 探测流信息 ==========
    printf("\n=== 第2步: 探测流信息 ===\n");

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "无法获取流信息\n");
        goto end;
    }

    printf("  流数量: %d\n", fmt_ctx->nb_streams);
    printf("  时长: %.2f 秒\n", fmt_ctx->duration / (double)AV_TIME_BASE);

    // 打印所有流的信息
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream *stream = fmt_ctx->streams[i];
        const char *type_str = av_get_media_type_string(stream->codecpar->codec_type);
        const AVCodecDescriptor *desc = avcodec_descriptor_get(stream->codecpar->codec_id);
        printf("  流 #%d: 类型=%s, 编码=%s\n",
               i, type_str ? type_str : "未知",
               desc ? desc->name : "未知");
    }

    // ========== 第3步: 找到视频流 ==========
    printf("\n=== 第3步: 查找视频流 ===\n");

    int video_stream_index = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
    }
    if (video_stream_index < 0) {
        fprintf(stderr, "找不到视频流\n");
        goto end;
    }

    AVStream *video_stream = fmt_ctx->streams[video_stream_index];
    AVCodecParameters *codecpar = video_stream->codecpar;
    printf("  视频流索引: %d\n", video_stream_index);
    printf("  分辨率: %dx%d\n", codecpar->width, codecpar->height);
    printf("  帧率: %.2f fps\n", av_q2d(video_stream->avg_frame_rate));

    // ========== 第4步: 打开解码器 ==========
    printf("\n=== 第4步: 打开解码器 ===\n");

    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "找不到解码器: %d\n", codecpar->codec_id);
        goto end;
    }
    printf("  解码器: %s (%s)\n", codec->name, codec->long_name);

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        fprintf(stderr, "无法分配解码器上下文\n");
        goto end;
    }

    // 将流参数拷贝到解码器上下文
    ret = avcodec_parameters_to_context(codec_ctx, codecpar);
    if (ret < 0) {
        fprintf(stderr, "无法拷贝编解码参数\n");
        goto end;
    }

    ret = avcodec_open2(codec_ctx, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "无法打开解码器\n");
        goto end;
    }
    printf("  像素格式: %s\n", av_get_pix_fmt_name(codec_ctx->pix_fmt));

    // ========== 第5步: 读取并解码 ==========
    printf("\n=== 第5步: 解码视频帧 (最多 %d 帧) ===\n", max_frames);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!pkt || !frame) {
        fprintf(stderr, "无法分配 packet/frame\n");
        goto end;
    }

    int frame_count = 0;

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        // 只处理视频流的包
        if (pkt->stream_index != video_stream_index) {
            av_packet_unref(pkt);
            continue;
        }

        printf("  读取 packet: size=%d, pts=%lld, dts=%lld\n",
               pkt->size, pkt->pts, pkt->dts);

        // 送入解码器
        ret = avcodec_send_packet(codec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) {
            fprintf(stderr, "  发送 packet 失败\n");
            continue;
        }

        // 从解码器取出帧 (一个packet可能产生多帧)
        while (ret >= 0) {
            ret = avcodec_receive_frame(codec_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0) {
                fprintf(stderr, "  解码错误\n");
                goto end;
            }

            printf("  解码帧 #%d: %dx%d, pts=%lld, key_frame=%d, pict_type=%c\n",
                   frame_count,
                   frame->width, frame->height,
                   frame->pts,
                   (frame->flags & AV_FRAME_FLAG_KEY) ? 1 : 0,
                   av_get_picture_type_char(frame->pict_type));

            // 保存前几帧为 PGM 图片 (灰度)
            save_frame_as_pgm(frame, frame->width, frame->height, frame_count);

            frame_count++;
            if (frame_count >= max_frames)
                break;
        }

        if (frame_count >= max_frames)
            break;
    }

    // 冲刷解码器 (flush)，取出缓冲的帧
    avcodec_send_packet(codec_ctx, NULL);
    while (frame_count < max_frames) {
        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
            break;
        printf("  冲刷帧 #%d\n", frame_count);
        save_frame_as_pgm(frame, frame->width, frame->height, frame_count);
        frame_count++;
    }

    printf("\n=== 完成: 共解码 %d 帧 ===\n", frame_count);

end:
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return 0;
}
