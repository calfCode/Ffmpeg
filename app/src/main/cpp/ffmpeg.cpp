//
// Created by hq on 2018/12/26.
//

#include "ffmpeg.h"



#include <csignal>
#include <csetjmp>

#ifdef __cplusplus
extern "C" {
#endif
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
#include <libavutil/channel_layout.h>
#include "libavutil/timestamp.h"
#include "libavutil/mem.h"
#include "libavutil/file.h"
#include "libavutil/opt.h"
#ifdef __cplusplus
}
#endif

#include <thread>

//#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG , TAG, __VA_ARGS__)
static AVFormatContext *pFormatCtx = NULL;
static int tryTimes = 0;
#define STREAM_DURATION   10.0
#define STREAM_FRAME_RATE 25 /* 25 images/s */
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */

#define SCALE_FLAGS SWS_BICUBIC

#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096

int getVersion() {
    int version = avutil_version();
    return version;
}

static AVFormatContext *fmt_ctx = NULL;
static AVCodecContext *video_dec_ctx = NULL, *audio_dec_ctx;
static int width, height;
static enum AVPixelFormat pix_fmt;
static AVStream *video_stream = NULL, *audio_stream = NULL;
static const char *src_filename = NULL;
static const char *video_dst_filename = NULL;
static const char *audio_dst_filename = NULL;
static FILE *video_dst_file = NULL;
static FILE *audio_dst_file = NULL;

static uint8_t *video_dst_data[4] = {NULL};
static int video_dst_linesize[4];
static int video_dst_bufsize;

static int video_stream_idx = -1, audio_stream_idx = -1;
static AVFrame *frame = NULL;
static AVPacket *pkt = NULL;
static int video_frame_count = 0;
static int audio_frame_count = 0;

static int output_video_frame(AVFrame *frame) {
    if (frame->width != width || frame->height != height ||
        frame->format != pix_fmt) {
        /* To handle this change, one could call av_image_alloc again and
         * decode the following frames into another rawvideo file. */
        LOGE("Error: Width, height and pixel format have to be "
             "constant in a rawvideo file, but the width, height or "
             "pixel format of the input video changed:\n"
             "old: width = %d, height = %d, format = %s\n"
             "new: width = %d, height = %d, format = %s\n",
             width, height, av_get_pix_fmt_name(pix_fmt),
             frame->width, frame->height,
             av_get_pix_fmt_name((AVPixelFormat) frame->format));
        return -1;
    }

    LOGD("video_frame n:%d\n",
         video_frame_count++);

    /* copy decoded frame to destination buffer:
     * this is required since rawvideo expects non aligned data */
    // 拷贝frame数据到缓存空间中
    av_image_copy2(video_dst_data, video_dst_linesize,
                   frame->data, frame->linesize,
                   pix_fmt, width, height);

    /* write to rawvideo file */
    // 把缓存空间写入到文件中
    fwrite(video_dst_data[0], 1, video_dst_bufsize, video_dst_file);
    return 0;
}

static int output_audio_frame(AVFrame *frame) {
    // 计算音频帧大小
    size_t unpadded_linesize =
            frame->nb_samples * av_get_bytes_per_sample((AVSampleFormat) frame->format);
    LOGD("audio_frame n:%d nb_samples:%d pts:%s\n",
         audio_frame_count++, frame->nb_samples,
         av_ts2timestr(frame->pts, &audio_dec_ctx->time_base));

    /* Write the raw audio data samples of the first plane. This works
     * fine for packed formats (e.g. AV_SAMPLE_FMT_S16). However,
     * most audio decoders output planar audio, which uses a separate
     * plane of audio samples for each channel (e.g. AV_SAMPLE_FMT_S16P).
     * In other words, this code will write only the first audio channel
     * in these cases.
     * You should use libswresample or libavfilter to convert the frame
     * to packed data. */
    // 写入文件中
    fwrite(frame->extended_data[0], 1, unpadded_linesize, audio_dst_file);

    return 0;
}

static int decode_packet(AVCodecContext *dec, const AVPacket *pkt) {
    int ret = 0;

    // submit the packet to the decoder
    ret = avcodec_send_packet(dec, pkt);// 发送数据到解码器
    if (ret < 0) {
        LOGE("Error submitting a packet for decoding (%s)\n", av_err2str(ret));
        return ret;
    }

    // get all the available frames from the decoder
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec, frame);// 从解码器结束解码后的数据
        if (ret < 0) {
            // those two return values are special and mean there is no output
            // frame available, but there were no errors during decoding
            // 这两种返回值特别的，代表没有输出帧，但是也代表在解码过程中没有错误，还需要读取数据进行继续解码
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                return 0;

            LOGE("Error during decoding (%s)\n", av_err2str(ret));
            return ret;
        }

        // write the frame data to output file
        if (dec->codec->type == AVMEDIA_TYPE_VIDEO)
            ret = output_video_frame(frame);// 输出解码后的视频帧
        else
            ret = output_audio_frame(frame);// 输出解码后的音频帧

        av_frame_unref(frame);//释放数据帧
    }

    return ret;
}

static int open_codec_context(int *stream_idx,
                              AVCodecContext **dec_ctx, AVFormatContext *fmt_ctx,
                              enum AVMediaType type) {
    int ret, stream_index;
    AVStream *st;
    const AVCodec *dec = NULL;

    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);// 根据媒体类型获取最匹配的流，返回流索引
    if (ret < 0) {
        LOGE("Could not find %s stream in input file '%s'\n",
             av_get_media_type_string(type), src_filename);
        return ret;
    } else {
        stream_index = ret;
        st = fmt_ctx->streams[stream_index];// 根据流索引获取流

        /* find decoder for the stream */
        dec = avcodec_find_decoder(st->codecpar->codec_id);// 根据流的codec id获取解码器
        if (!dec) {
            LOGE("Failed to find %s codec\n",
                 av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }

        /* Allocate a codec context for the decoder */
        *dec_ctx = avcodec_alloc_context3(dec);// 获取解码器的上下文
        if (!*dec_ctx) {
            LOGE("Failed to allocate the %s codec context\n",
                 av_get_media_type_string(type));
            return AVERROR(ENOMEM);
        }

        /* Copy codec parameters from input stream to output codec context */
        // 复制流的codec参数到解码器的上下文
        if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
            LOGE("Failed to copy %s codec parameters to decoder context\n",
                 av_get_media_type_string(type));
            return ret;
        }

        /* Init the decoders */
        // 初始化解码器
        if ((ret = avcodec_open2(*dec_ctx, dec, NULL)) < 0) {
            LOGE("Failed to open %s codec\n",
                 av_get_media_type_string(type));
            return ret;
        }
        *stream_idx = stream_index;
    }

    return 0;
}

static int get_format_from_sample_fmt(const char **fmt,
                                      enum AVSampleFormat sample_fmt) {
    int i;
    struct sample_fmt_entry {
        enum AVSampleFormat sample_fmt;
        const char *fmt_be, *fmt_le;
    } sample_fmt_entries[] = {
            {AV_SAMPLE_FMT_U8,  "u8",    "u8"},
            {AV_SAMPLE_FMT_S16, "s16be", "s16le"},
            {AV_SAMPLE_FMT_S32, "s32be", "s32le"},
            {AV_SAMPLE_FMT_FLT, "f32be", "f32le"},
            {AV_SAMPLE_FMT_DBL, "f64be", "f64le"},
    };
    *fmt = NULL;

    for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
        struct sample_fmt_entry *entry = &sample_fmt_entries[i];
        if (sample_fmt == entry->sample_fmt) {
            *fmt = AV_NE(entry->fmt_be, entry->fmt_le);
            return 0;
        }
    }

    LOGE(
            "sample format %s is not supported as output format\n",
            av_get_sample_fmt_name(sample_fmt));
    return -1;
}

int example_demux(const char *filePath) {
    int ret = 0;

    src_filename = filePath;
    video_dst_filename = "/data/user/0/com.smartdevice.ffmpeg/app_video/out.v"; // 视频流保存路径
    audio_dst_filename = "/data/user/0/com.smartdevice.ffmpeg/app_video/out.a";// 音频流保存路径

    /* open input file, and allocate format context */
    // 打开文件，分配format上下文
    if (avformat_open_input(&fmt_ctx, src_filename, NULL, NULL) < 0) {
        LOGE("Could not open source file %s\n", src_filename);
        exit(1);
    }

    /* retrieve stream information */
    // 获取文件的流信息
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        LOGE("Could not find stream information\n");
        exit(1);
    }
    // 获取视频流 codec上下文
    if (open_codec_context(&video_stream_idx, &video_dec_ctx, fmt_ctx, AVMEDIA_TYPE_VIDEO) >= 0) {
        video_stream = fmt_ctx->streams[video_stream_idx];// 视频流

        video_dst_file = fopen(video_dst_filename, "wb+");// 打开视频流写入文件
        if (!video_dst_file) {
            LOGE("Could not open destination file %s\n", video_dst_filename);
            ret = 1;
            goto end;
        }

        /* allocate image where the decoded image will be put */
        // 分配图像存储空间，用于存储视频解码后的数据
        width = video_dec_ctx->width;
        height = video_dec_ctx->height;
        pix_fmt = video_dec_ctx->pix_fmt;
        ret = av_image_alloc(video_dst_data, video_dst_linesize,
                             width, height, pix_fmt, 1);
        if (ret < 0) {
            LOGE("Could not allocate raw video buffer\n");
            goto end;
        }
        video_dst_bufsize = ret;
    }
    // 获取音频流 codec上下文
    if (open_codec_context(&audio_stream_idx, &audio_dec_ctx, fmt_ctx, AVMEDIA_TYPE_AUDIO) >= 0) {
        audio_stream = fmt_ctx->streams[audio_stream_idx];// 音频流
        audio_dst_file = fopen(audio_dst_filename, "wb+");// 打开音频流写入文件
        if (!audio_dst_file) {
            LOGE("Could not open destination file %s\n", audio_dst_filename);
            ret = 1;
            goto end;
        }
    }

    /* dump input information to stderr */
    av_dump_format(fmt_ctx, 0, src_filename, 0);

    if (!audio_stream && !video_stream) {
        LOGE("Could not find audio or video stream in the input, aborting\n");
        ret = 1;
        goto end;
    }

    frame = av_frame_alloc();// 分配frame
    if (!frame) {
        LOGE("Could not allocate frame\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    pkt = av_packet_alloc(); // 分配packet
    if (!pkt) {
        LOGE("Could not allocate packet\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if (video_stream)
        LOGD("Demuxing video from file '%s' into '%s'\n", src_filename, video_dst_filename);
    if (audio_stream)
        LOGD("Demuxing audio from file '%s' into '%s'\n", src_filename, audio_dst_filename);

    /* read frames from the file */
    while (av_read_frame(fmt_ctx, pkt) >= 0) {// 从文件读取帧数据
        // check if the packet belongs to a stream we are interested in, otherwise
        // skip it
        if (pkt->stream_index == video_stream_idx)
            ret = decode_packet(video_dec_ctx, pkt);
        else if (pkt->stream_index == audio_stream_idx)
            ret = decode_packet(audio_dec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            break;
    }

    /* flush the decoders */
    // 刷新解码器
    if (video_dec_ctx)
        decode_packet(video_dec_ctx, NULL);
    if (audio_dec_ctx)
        decode_packet(audio_dec_ctx, NULL);

    LOGD("Demuxing succeeded.\n");

    if (video_stream) {
        // 提示如何使用ffplay播放裸视频流
        LOGD("Play the output video file with the command:\n"
             "ffplay -f rawvideo -pix_fmt %s -video_size %dx%d %s\n",
             av_get_pix_fmt_name(pix_fmt), width, height,
             video_dst_filename);
    }

    if (audio_stream) {
        enum AVSampleFormat sfmt = audio_dec_ctx->sample_fmt;
        int n_channels = audio_dec_ctx->ch_layout.nb_channels;
        const char *fmt;

        if (av_sample_fmt_is_planar(sfmt)) {
            const char *packed = av_get_sample_fmt_name(sfmt);
            LOGD("Warning: the sample format the decoder produced is planar "
                 "(%s). This example will output the first channel only.\n",
                 packed ? packed : "?");
            sfmt = av_get_packed_sample_fmt(sfmt);
            n_channels = 1;
        }

        if ((ret = get_format_from_sample_fmt(&fmt, sfmt)) < 0)
            goto end;
// 提示如何使用ffplay播放裸音频流
        LOGD("Play the output audio file with the command:\n"
             "ffplay -f %s -ac %d -ar %d %s\n",
             fmt, n_channels, audio_dec_ctx->sample_rate,
             audio_dst_filename);
    }

    // 释放资源
    end:
    avcodec_free_context(&video_dec_ctx);
    avcodec_free_context(&audio_dec_ctx);
    avformat_close_input(&fmt_ctx);
    if (video_dst_file)
        fclose(video_dst_file);
    if (audio_dst_file)
        fclose(audio_dst_file);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_free(video_dst_data[0]);

    return ret < 0;
}

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt, const char *tag) {
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    LOGD("%s: pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
         tag,
         av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
         av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
         av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
         pkt->stream_index);
}

int example_remux(const char *filePath) {
    const AVOutputFormat *ofmt = NULL;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVPacket *pkt = NULL;
    const char *in_filename, *out_filename;
    int ret, i;
    int stream_index = 0;
    int *stream_mapping = NULL;
    int stream_mapping_size = 0;


    in_filename = filePath;
    out_filename = "/storage/emulated/0/DCIM/test701_0seconds.mp4";

    pkt = av_packet_alloc();// 分配包
    if (!pkt) {
        LOGE("Could not allocate AVPacket\n");
        return 1;
    }
    //读取文件头，获取封装格式相关信息存储到ifmt_ctx中
    if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0) {
        LOGE("Could not open input file '%s'", in_filename);
        goto end;
    }
    // 解码一段数据，获取流相关信息，将取到的流信息填入AVFormatContext.streams中。
    // AVFormatContext.streams是一个指针数组，数组大小是AVFormatContext.nb_streams
    if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) { // 查找输入文件的流
        LOGE("Failed to retrieve input stream information");
        goto end;
    }

    av_dump_format(ifmt_ctx, 0, in_filename, 0);
// 2.1 分配输出ctx
    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_filename);// 分配输出AVFormatContext
    if (!ofmt_ctx) {
        LOGE("Could not create output context\n");
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    stream_mapping_size = ifmt_ctx->nb_streams;
    stream_mapping = (int *) av_calloc(stream_mapping_size, sizeof(*stream_mapping));
    if (!stream_mapping) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ofmt = ofmt_ctx->oformat;

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *out_stream;
        AVStream *in_stream = ifmt_ctx->streams[i];
        AVCodecParameters *in_codecpar = in_stream->codecpar; /* 用于记录编码后的流信息，即通道中存储的流的编码信息 */

        if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            stream_mapping[i] = -1;
            continue;
        }
        int64_t timeStart = av_rescale_q(0 * AV_TIME_BASE, AV_TIME_BASE_Q, in_stream->time_base);
        av_seek_frame(ifmt_ctx, stream_index, timeStart, AVSEEK_FLAG_BACKWARD);
        stream_mapping[i] = stream_index++;

        // 2.2 将一个新流(out_stream)添加到输出文件(ofmt_ctx)
        out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            LOGE("Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }
        // 2.3 将当前输入流中的参数拷贝到输出流中
        ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
        if (ret < 0) {
            LOGE("Failed to copy codec parameters\n");
            goto end;
        }
        out_stream->codecpar->codec_tag = 0;
    }
    av_dump_format(ofmt_ctx, 0, out_filename, 1);

    if (!(ofmt->flags & AVFMT_NOFILE)) {
        // 2.4 创建并初始化一个AVIOContext，用以访问URL(out_filename)指定的资源
        ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            LOGE("Could not open output file '%s'", out_filename);
            goto end;
        }
    }
// 3.1 写输出文件头
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        LOGE("Error occurred when opening output file\n");
        goto end;
    }


    while (1) {
        // AVStream.time_base是AVPacket中pts和dts的时间单位，输入流与输出流中time_base按如下方式确定：
        // 对于输入流：打开输入文件后，调用avformat_find_stream_info()可获取到每个流中的time_base
        // 对于输出流：打开输出文件后，调用avformat_write_header()可根据输出文件封装格式确定每个流的time_base并写入输出文件中

        AVStream *in_stream, *out_stream;
        // 3.2
        // 从输入流读取一个packet，对于视频来说，一个packet只包含一个视频帧；
        // 对于音频来说，若是帧长固定的格式则一个packet可包含整数个音频帧，
        // 若是帧长可变的格式则一个packet只包含一个音频帧

        ret = av_read_frame(ifmt_ctx, pkt); // 从输入文件读取frame
        if (ret < 0)
            break;

        in_stream = ifmt_ctx->streams[pkt->stream_index];
        if (pkt->stream_index >= stream_mapping_size ||
            stream_mapping[pkt->stream_index] < 0) {
            av_packet_unref(pkt);
            continue;
        }

        pkt->stream_index = stream_mapping[pkt->stream_index];
        out_stream = ofmt_ctx->streams[pkt->stream_index];
        log_packet(ifmt_ctx, pkt, "in");

        /* copy packet */
        /* 3.3 更新packet中的pts和dts
   关于AVStream.time_base的说明：
   输入：输入流中含有time_base，在avformat_find_stream_info()中可取到每个流中的time_base
   输出：avformat_write_header()会根据输出的封装格式确定每个流的time_base并写入文件中
   AVPacket.pts和AVPacket.dts的单位是AVStream.time_base，不同的封装格式其AVStream.time_base不同
   所以输出文件中，每个packet需要根据输出封装格式重新计算pts和dts
 */
        /* 将packet中的各时间值从输入流封装格式时间基转换到输出流封装格式时间基 */
        av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
        pkt->pos = -1;
        log_packet(ofmt_ctx, pkt, "out");


        // 3.4 将packet写入输出，确保输出媒体中不同流的packet能按照dts增长的顺序正确交织
        ret = av_interleaved_write_frame(ofmt_ctx, pkt);
        /* pkt is now blank (av_interleaved_write_frame() takes ownership of
         * its contents and resets pkt), so that no unreferencing is necessary.
         * This would be different if one used av_write_frame(). */
        if (ret < 0) {
            LOGE("Error muxing packet\n");
            break;
        }
    }

    av_write_trailer(ofmt_ctx);
    end:
    av_packet_free(&pkt);

    avformat_close_input(&ifmt_ctx);

    /* close output */
    if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
        avio_closep(&ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);

    av_freep(&stream_mapping);

    if (ret < 0 && ret != AVERROR_EOF) {
        LOGE("Error occurred: %s\n", av_err2str(ret));
        return 1;
    }

    return 0;
}


struct buffer_data {
    uint8_t *ptr;
    size_t size; ///< size left in the buffer
};

static int read_packet(void *opaque, uint8_t *buf, int buf_size) {
    struct buffer_data *bd = (struct buffer_data *) opaque;
    buf_size = FFMIN(buf_size, bd->size);

    if (!buf_size)
        return AVERROR_EOF;
    LOGD("ptr:%p size:%zu\n", bd->ptr, bd->size);

    /* copy internal buffer data to buf */
    memcpy(buf, bd->ptr, buf_size);
    bd->ptr += buf_size;
    bd->size -= buf_size;

    return buf_size;
}

int example_avio_reading(const char *filePath) {
    AVFormatContext *fmt_ctx = NULL;
    AVIOContext *avio_ctx = NULL;
    uint8_t *buffer = NULL, *avio_ctx_buffer = NULL;
    size_t buffer_size, avio_ctx_buffer_size = 4096;
    char *input_filename = NULL;
    int ret = 0;
    struct buffer_data bd = {0};


    input_filename = (char *) filePath;

    /* slurp file content into buffer */
    // 将输入文件的数据映射到内存buffer中
    ret = av_file_map(input_filename, &buffer, &buffer_size, 0, NULL);
    if (ret < 0)
        goto end;

    /* fill opaque structure used by the AVIOContext read callback */
    bd.ptr = buffer;
    bd.size = buffer_size;
    // 申请AVFormatContext
    if (!(fmt_ctx = avformat_alloc_context())) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    avio_ctx_buffer = (uint8_t *) av_malloc(avio_ctx_buffer_size);
    if (!avio_ctx_buffer) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    // 申请AVIOContext,同时将内存数据读取的回调接口注册给AVIOContext
    avio_ctx = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size,
                                  0, &bd, &read_packet, NULL, NULL);
    if (!avio_ctx) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    fmt_ctx->pb = avio_ctx;
    // 打开AVFormatContext
    ret = avformat_open_input(&fmt_ctx, NULL, NULL, NULL);
    if (ret < 0) {
        LOGE("Could not open input\n");
        goto end;
    }
    // 查找流
    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        LOGE("Could not find stream information\n");
        goto end;
    }

    av_dump_format(fmt_ctx, 0, input_filename, 0);

    end:
    avformat_close_input(&fmt_ctx);

    /* note: the internal buffer could have changed, and be != avio_ctx_buffer */
    if (avio_ctx)
        av_freep(&avio_ctx->buffer);
    avio_context_free(&avio_ctx);

    av_file_unmap(buffer, buffer_size);

    if (ret < 0) {
        LOGE("Error occurred: %s\n", av_err2str(ret));
        return 1;
    }

    return 0;
}

int example_decode (const char *filePath)
{
    int ret = 0;


    src_filename = filePath;
    video_dst_filename = "/data/user/0/com.smartdevice.ffmpeg/app_video/out.v"; // 视频流保存路径
    audio_dst_filename = "/data/user/0/com.smartdevice.ffmpeg/app_video/out.a";// 音频流保存路径

    /* open input file, and allocate format context */
    // 关联fmt_ctx到src_filename文件
    if (avformat_open_input(&fmt_ctx, src_filename, NULL, NULL) < 0) {
        LOGE("Could not open source file %s\n", src_filename);
        exit(1);
    }

    /* retrieve stream information */
    // 获取媒体 流信息
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        LOGE("Could not find stream information\n");
        exit(1);
    }

    if (open_codec_context(&video_stream_idx, &video_dec_ctx, fmt_ctx, AVMEDIA_TYPE_VIDEO) >= 0) {
        video_stream = fmt_ctx->streams[video_stream_idx];// 获取视频流
        video_dst_file = fopen(video_dst_filename, "wb");// 打开文件
        if (!video_dst_file) {
            LOGE("Could not open destination file %s\n", video_dst_filename);
            ret = 1;
            goto end;
        }

        /* allocate image where the decoded image will be put */
        width = video_dec_ctx->width;
        height = video_dec_ctx->height;
        pix_fmt = video_dec_ctx->pix_fmt;
        ret = av_image_alloc(video_dst_data, video_dst_linesize,
                             width, height, pix_fmt, 1);// 分配缓存空间，用于缓存解码后视频帧
        if (ret < 0) {
            LOGE("Could not allocate raw video buffer\n");
            goto end;
        }
        video_dst_bufsize = ret;
    }

    if (open_codec_context(&audio_stream_idx, &audio_dec_ctx, fmt_ctx, AVMEDIA_TYPE_AUDIO) >= 0) {
        audio_stream = fmt_ctx->streams[audio_stream_idx];// 获取音频流
        audio_dst_file = fopen(audio_dst_filename, "wb");// 打开文件
        if (!audio_dst_file) {
            LOGE("Could not open destination file %s\n", audio_dst_filename);
            ret = 1;
            goto end;
        }
    }

    /* dump input information to stderr */
    av_dump_format(fmt_ctx, 0, src_filename, 0);

    if (!audio_stream && !video_stream) {
        LOGE("Could not find audio or video stream in the input, aborting\n");
        ret = 1;
        goto end;
    }

    frame = av_frame_alloc();// 分配用于存储解码后的空间
    if (!frame) {
        LOGE("Could not allocate frame\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    pkt = av_packet_alloc();// 分配用于存储 读取媒体文件的空间
    if (!pkt) {
        LOGE("Could not allocate packet\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if (video_stream)
        LOGD("Demuxing video from file '%s' into '%s'\n", src_filename, video_dst_filename);
    if (audio_stream)
        LOGD("Demuxing audio from file '%s' into '%s'\n", src_filename, audio_dst_filename);

    /* read frames from the file */
    while (av_read_frame(fmt_ctx, pkt) >= 0) { // 从文件读取数
        // check if the packet belongs to a stream we are interested in, otherwise
        // skip it
        // 根据数据类型，分别进行音视频解码
        if (pkt->stream_index == video_stream_idx)
            ret = decode_packet(video_dec_ctx, pkt);
        else if (pkt->stream_index == audio_stream_idx)
            ret = decode_packet(audio_dec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            break;
    }

    /* flush the decoders */
    if (video_dec_ctx)
        decode_packet(video_dec_ctx, NULL);
    if (audio_dec_ctx)
        decode_packet(audio_dec_ctx, NULL);

    LOGD("Demuxing succeeded.\n");

    if (video_stream) {
        LOGD("Play the output video file with the command:\n"
               "ffplay -f rawvideo -pix_fmt %s -video_size %dx%d %s\n",
               av_get_pix_fmt_name(pix_fmt), width, height,
               video_dst_filename);
    }

    if (audio_stream) {
        enum AVSampleFormat sfmt = audio_dec_ctx->sample_fmt;
        int n_channels = audio_dec_ctx->ch_layout.nb_channels;
        const char *fmt;

        if (av_sample_fmt_is_planar(sfmt)) {
            const char *packed = av_get_sample_fmt_name(sfmt);
            LOGD("Warning: the sample format the decoder produced is planar "
                   "(%s). This example will output the first channel only.\n",
                   packed ? packed : "?");
            sfmt = av_get_packed_sample_fmt(sfmt);
            n_channels = 1;
        }

        if ((ret = get_format_from_sample_fmt(&fmt, sfmt)) < 0)
            goto end;

        LOGD("Play the output audio file with the command:\n"
               "ffplay -f %s -ac %d -ar %d %s\n",
               fmt, n_channels, audio_dec_ctx->sample_rate,
               audio_dst_filename);
    }

    end:
    avcodec_free_context(&video_dec_ctx);
    avcodec_free_context(&audio_dec_ctx);
    avformat_close_input(&fmt_ctx);
    if (video_dst_file)
        fclose(video_dst_file);
    if (audio_dst_file)
        fclose(audio_dst_file);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_free(video_dst_data[0]);

    return ret < 0;
}

/* check that a given sample format is supported by the encoder */
// 检查采样格式是否在编码器支持的采样列表中
static int check_sample_fmt(const AVCodec *codec, enum AVSampleFormat sample_fmt)
{
    const enum AVSampleFormat *p = codec->sample_fmts;

    while (*p != AV_SAMPLE_FMT_NONE) {
        if (*p == sample_fmt)
            return 1;
        p++;
    }
    return 0;
}

/* just pick the highest supported samplerate */
// 选择采样率
static int select_sample_rate(const AVCodec *codec)
{
    const int *p;
    int best_samplerate = 0;

    if (!codec->supported_samplerates)
        return 44100;

    p = codec->supported_samplerates;
    while (*p) {
        if (!best_samplerate || abs(44100 - *p) < abs(44100 - best_samplerate))
            best_samplerate = *p;
        p++;
    }
    return best_samplerate;
}

/* select layout with the highest channel count */
// 选择声音布局
static int select_channel_layout(const AVCodec *codec, AVChannelLayout *dst)
{
    const AVChannelLayout *p, *best_ch_layout;
    int best_nb_channels   = 0;

    if (!codec->ch_layouts){
         AVChannelLayout src = AV_CHANNEL_LAYOUT_STEREO;
        return av_channel_layout_copy(dst,&src);
    }


    p = codec->ch_layouts;
    while (p->nb_channels) {
        int nb_channels = p->nb_channels;

        if (nb_channels > best_nb_channels) {
            best_ch_layout   = p;
            best_nb_channels = nb_channels;
        }
        p++;
    }
    return av_channel_layout_copy(dst, best_ch_layout);
}

static void encode_audio(AVCodecContext *ctx, AVFrame *frame, AVPacket *pkt,
                         FILE *output)
{
    int ret;

    /* send the frame for encoding */
    ret = avcodec_send_frame(ctx, frame);// 发送frame给编码器
    if (ret < 0) {
        LOGE("Error sending the frame to the encoder\n");
        exit(1);
    }

    /* read all the available output packets (in general there may be any
     * number of them */
    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx, pkt);// 从编码器接收编码后的数据
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            LOGE("Error encoding audio frame\n");
            exit(1);
        }

        fwrite(pkt->data, 1, pkt->size, output);// 写入到文件中
        av_packet_unref(pkt);
    }
}

int example_audio_encode(const char *filePath)
{
    const char *filename;
    const AVCodec *codec;
    AVCodecContext *c= NULL;
    AVFrame *frame;
    AVPacket *pkt;
    int i, j, k, ret;
    FILE *f;
    uint16_t *samples;
    float t, tincr;
   
    filename = filePath;
    /* find the MP2 encoder */
    codec = avcodec_find_encoder(AV_CODEC_ID_MP2);// 查找编码器
    if (!codec) {
        LOGE("Codec not found\n");
        exit(1);
    }

    c = avcodec_alloc_context3(codec); // 分配codec context
    if (!c) {
        LOGE("Could not allocate audio codec context\n");
        exit(1);
    }

    /* put sample parameters */
    // 设置编码参数
    c->bit_rate = 64000;

    /* check that the encoder supports s16 pcm input */
    c->sample_fmt = AV_SAMPLE_FMT_S16;
    if (!check_sample_fmt(codec, c->sample_fmt)) {
        LOGE("Encoder does not support sample format %s",
                av_get_sample_fmt_name(c->sample_fmt));
        exit(1);
    }

    /* select other audio parameters supported by the encoder */
    c->sample_rate    = select_sample_rate(codec);
    ret = select_channel_layout(codec, &c->ch_layout);
    if (ret < 0)
        exit(1);

    /* open it */
    // 打开编码器
    if (avcodec_open2(c, codec, NULL) < 0) {
        LOGE("Could not open codec\n");
        exit(1);
    }

    // 打开文件
    f = fopen(filename, "wb");
    if (!f) {
        LOGE("Could not open %s\n", filename);
        exit(1);
    }

    /* packet for holding encoded output */
    pkt = av_packet_alloc();
    if (!pkt) {
        LOGE("could not allocate the packet\n");
        exit(1);
    }

    /* frame containing input raw audio */
    // 设置frame 参数
    frame = av_frame_alloc();
    if (!frame) {
        LOGE("Could not allocate audio frame\n");
        exit(1);
    }
    frame->nb_samples     = c->frame_size;
    frame->format         = c->sample_fmt;
    ret = av_channel_layout_copy(&frame->ch_layout, &c->ch_layout);// 从编码器上下文拷贝通道布局
    if (ret < 0)
        exit(1);

    /* allocate the data buffers */
    // 分配内存
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        LOGE("Could not allocate audio data buffers\n");
        exit(1);
    }

    /* encode a single tone sound */
    t = 0;
    tincr = 2 * M_PI * 440.0 / c->sample_rate;
    for (i = 0; i < 200; i++) {
        /* make sure the frame is writable -- makes a copy if the encoder
         * kept a reference internally */
        ret = av_frame_make_writable(frame); // 设置frame 为可写入的
        if (ret < 0)
            exit(1);
        samples = (uint16_t*)frame->data[0];// samples 指向frame data区

        for (j = 0; j < c->frame_size; j++) {
            samples[2*j] = (int)(sin(t) * 10000);

            for (k = 1; k < c->ch_layout.nb_channels; k++)
                samples[2*j + k] = samples[2*j];
            t += tincr;
        }
        encode_audio(c, frame, pkt, f);// 编码
    }

    /* flush the encoder */
    encode_audio(c, NULL, pkt, f);

    fclose(f);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&c);

    return 0;
}


static void encode_video(AVCodecContext *enc_ctx, AVFrame *frame, AVPacket *pkt,
                   FILE *outfile)
{
    int ret;

    /* send the frame to the encoder */
    if (frame)
        LOGD("Send frame %3" PRId64"\n", frame->pts);

    ret = avcodec_send_frame(enc_ctx, frame);// 发送数据给编码器
    if (ret < 0) {
        LOGE("Error sending a frame for encoding\n");
        exit(1);
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(enc_ctx, pkt);// 获取编码后的数据
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            LOGE("Error during encoding\n");
            exit(1);
        }

        LOGD("Write packet %3" PRId64" (size=%5d)\n", pkt->pts, pkt->size);
        fwrite(pkt->data, 1, pkt->size, outfile);// 写入数据到文件
        av_packet_unref(pkt);
    }
}

int example_video_encode(const char *filePath)
{
    const char *filename, *codec_name;
    const AVCodec *codec;
    AVCodecContext *c= NULL;
    int i, ret, x, y;
    FILE *f;
    AVFrame *frame;
    AVPacket *pkt;
    uint8_t endcode[] = { 0, 0, 1, 0xb7 };


    filename = filePath;
    codec_name = "mpeg1video";

    /* find the mpeg1video encoder */
    codec = avcodec_find_encoder_by_name(codec_name);//根据名字查找编码器
    if (!codec) {
        LOGE("Codec '%s' not found\n", codec_name);
        exit(1);
    }

    c = avcodec_alloc_context3(codec);// 分配codec上下文
    if (!c) {
        LOGE("Could not allocate video codec context\n");
        exit(1);
    }

    pkt = av_packet_alloc(); // 分配packet
    if (!pkt)
        exit(1);

    /* put sample parameters */
    // 设置编码器参数
    c->bit_rate = 400000;// 比特率
    /* resolution must be a multiple of two */
    c->width = 352; //宽
    c->height = 288;// 高
    /* frames per second */

    c->time_base = (AVRational){1, 25};
    c->framerate = (AVRational){25, 1};//帧率

    /* emit one intra frame every ten frames
     * check frame pict_type before passing frame
     * to encoder, if frame->pict_type is AV_PICTURE_TYPE_I
     * then gop_size is ignored and the output of encoder
     * will always be I frame irrespective to gop_size
     */
    c->gop_size = 10;// 10帧一个GOP
    c->max_b_frames = 1;// 最大可以包含一个B帧
    c->pix_fmt = AV_PIX_FMT_YUV420P;// 格式为YUV420P

    if (codec->id == AV_CODEC_ID_H264){
        av_opt_set(c->priv_data, "preset", "slow", 0);
    }
      

    /* open it */
    // 打开编码器
    ret = avcodec_open2(c, codec, NULL);
    if (ret < 0) {
        LOGE("Could not open codec: %s\n", av_err2str(ret));
        exit(1);
    }

    f = fopen(filename, "wb");
    if (!f) {
        LOGE("Could not open %s\n", filename);
        exit(1);
    }

    frame = av_frame_alloc();
    if (!frame) {
        LOGE("Could not allocate video frame\n");
        exit(1);
    }
    frame->format = c->pix_fmt;
    frame->width  = c->width;
    frame->height = c->height;
    // 分配frame 的数据空间
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        LOGE("Could not allocate the video frame data\n");
        exit(1);
    }

    /* encode 1 second of video */
    for (i = 0; i < 25; i++) {
        fflush(stdout);

        /* Make sure the frame data is writable.
           On the first round, the frame is fresh from av_frame_get_buffer()
           and therefore we know it is writable.
           But on the next rounds, encode() will have called
           avcodec_send_frame(), and the codec may have kept a reference to
           the frame in its internal structures, that makes the frame
           unwritable.
           av_frame_make_writable() checks that and allocates a new buffer
           for the frame only if necessary.
         */
        ret = av_frame_make_writable(frame);
        if (ret < 0)
            exit(1);

        /* Prepare a dummy image.
           In real code, this is where you would have your own logic for
           filling the frame. FFmpeg does not care what you put in the
           frame.
         */
        /* Y */
        for (y = 0; y < c->height; y++) {
            for (x = 0; x < c->width; x++) {
                frame->data[0][y * frame->linesize[0] + x] = x + y + i * 3;
            }
        }

        /* Cb and Cr */
        for (y = 0; y < c->height/2; y++) {
            for (x = 0; x < c->width/2; x++) {
                frame->data[1][y * frame->linesize[1] + x] = 128 + y + i * 2;
                frame->data[2][y * frame->linesize[2] + x] = 64 + x + i * 5;
            }
        }

        frame->pts = i;

        /* encode the image */
        // 编码视频
        encode_video(c, frame, pkt, f);
    }

    /* flush the encoder */
     // 刷新编码器
    encode_video(c, NULL, pkt, f);

    /* Add sequence end code to have a real MPEG file.
       It makes only sense because this tiny examples writes packets
       directly. This is called "elementary stream" and only works for some
       codecs. To create a valid file, you usually need to write packets
       into a proper file format or protocol; see mux.c.
     */
    //  写入特定的数据结尾
    if (codec->id == AV_CODEC_ID_MPEG1VIDEO || codec->id == AV_CODEC_ID_MPEG2VIDEO)
        fwrite(endcode, 1, sizeof(endcode), f);
    fclose(f);

    avcodec_free_context(&c);
    av_frame_free(&frame);
    av_packet_free(&pkt);

    return 0;
}



#define INBUF_SIZE 4096

static void pgm_save(unsigned char *buf, int wrap, int xsize, int ysize,
                     char *filename)
{
    FILE *f;
    int i;

    f = fopen(filename,"wb");
    fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);
    for (i = 0; i < ysize; i++)
        fwrite(buf + i * wrap, 1, xsize, f);
    fclose(f);
}

static void decode_video(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt,
                   const char *filename)
{
    char buf[1024];
    int ret;

    ret = avcodec_send_packet(dec_ctx, pkt);// 发送数据到解码器
    if (ret < 0) {
        LOGE("Error sending a packet for decoding\n");
        exit(1);
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame); // 接收解码器的输出数据
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            LOGE("Error during decoding\n");
            exit(1);
        }

        LOGD("saving frame %3" PRId64"\n", dec_ctx->frame_num);
        fflush(stdout);

        /* the picture is allocated by the decoder. no need to
           free it */
        snprintf(buf, sizeof(buf), "%s-%" PRId64, filename, dec_ctx->frame_num);
        pgm_save(frame->data[0], frame->linesize[0],
                 frame->width, frame->height, buf);// 保存文件
    }
}

int example_video_decode(const char *filePath)
{
    const char *filename, *outfilename;
    const AVCodec *codec;
    AVCodecParserContext *parser;
    AVCodecContext *c= NULL;
    FILE *f;
    AVFrame *frame;
    uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    uint8_t *data;
    size_t   data_size;
    int ret;
    int eof;
    AVPacket *pkt;

    filename    = filePath;
    outfilename = "/data/user/0/com.smartdevice.ffmpeg/app_video/decode.v";

    pkt = av_packet_alloc();
    if (!pkt)
        exit(1);

    /* set end of buffer to 0 (this ensures that no overreading happens for damaged MPEG streams) */
    memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    /* find the MPEG-1 video decoder */
    codec = avcodec_find_decoder(AV_CODEC_ID_MPEG1VIDEO); // 查找解码器
    if (!codec) {
        LOGE("Codec not found\n");
        exit(1);
    }

    parser = av_parser_init(codec->id);// 根据codec id初始化AVCodecParserContext
    if (!parser) {
        LOGE("parser not found\n");
        exit(1);
    }

    c = avcodec_alloc_context3(codec);// 分配AVCodecContext
    if (!c) {
        LOGE("Could not allocate video codec context\n");
        exit(1);
    }

    /* For some codecs, such as msmpeg4 and mpeg4, width and height
       MUST be initialized there because this information is not
       available in the bitstream. */

    /* open it */
    if (avcodec_open2(c, codec, NULL) < 0) { // 打开解码器
        LOGE("Could not open codec\n");
        exit(1);
    }

    f = fopen(filename, "rb");
    if (!f) {
        LOGE("Could not open %s\n", filename);
        exit(1);
    }

    frame = av_frame_alloc();
    if (!frame) {
        LOGE("Could not allocate video frame\n");
        exit(1);
    }

    do {
        /* read raw data from the input file */
        data_size = fread(inbuf, 1, INBUF_SIZE, f);// 读取INBUF_SIZE个字节
        if (ferror(f))
            break;
        eof = !data_size;

        /* use the parser to split the data into frames */
        data = inbuf;
        while (data_size > 0 || eof) {
            ret = av_parser_parse2(parser, c, &pkt->data, &pkt->size,
                                   data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (ret < 0) {
                LOGE("Error while parsing\n");
                exit(1);
            }
            data      += ret;
            data_size -= ret;

            if (pkt->size)
                decode_video(c, frame, pkt, outfilename);
            else if (eof)
                break;
        }
    } while (!eof);

    /* flush the decoder */
    decode_video(c, frame, NULL, outfilename);

    fclose(f);

    av_parser_close(parser);
    avcodec_free_context(&c);
    av_frame_free(&frame);
    av_packet_free(&pkt);

    return 0;
}


static void decode_audio(AVCodecContext *dec_ctx, AVPacket *pkt, AVFrame *frame,
                   FILE *outfile)
{
    int i, ch;
    int ret, data_size;

    /* send the packet with the compressed data to the decoder */
    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        LOGE("Error submitting the packet to the decoder\n");
        exit(1);
    }

    /* read all the output frames (in general there may be any number of them */
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            LOGE("Error during decoding\n");
            exit(1);
        }
        data_size = av_get_bytes_per_sample(dec_ctx->sample_fmt);
        if (data_size < 0) {
            /* This should not occur, checking just for paranoia */
            LOGE("Failed to calculate data size\n");
            exit(1);
        }
        for (i = 0; i < frame->nb_samples; i++)
            for (ch = 0; ch < dec_ctx->ch_layout.nb_channels; ch++)
                fwrite(frame->data[ch] + data_size*i, 1, data_size, outfile);
    }
}

int example_audio_decode(const char *filePath)
{
    const char *outfilename, *filename;
    const AVCodec *codec;
    AVCodecContext *c= NULL;
    AVCodecParserContext *parser = NULL;
    int len, ret;
    FILE *f, *outfile;
    uint8_t inbuf[AUDIO_INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    uint8_t *data;
    size_t   data_size;
    AVPacket *pkt;
    AVFrame *decoded_frame = NULL;
    enum AVSampleFormat sfmt;
    int n_channels = 0;
    const char *fmt;
    
    filename    = filePath;
    outfilename = "/data/user/0/com.smartdevice.ffmpeg/app_video/decode.a";
    pkt = av_packet_alloc();

    /* find the MPEG audio decoder */
    codec = avcodec_find_decoder(AV_CODEC_ID_MP2);// 查找解码器
    if (!codec) {
        LOGE("Codec not found\n");
        exit(1);
    }

    parser = av_parser_init(codec->id);
    if (!parser) {
        LOGE("Parser not found\n");
        exit(1);
    }

    c = avcodec_alloc_context3(codec);
    if (!c) {
        LOGE("Could not allocate audio codec context\n");
        exit(1);
    }

    /* open it */
     // 打开解码器
    if (avcodec_open2(c, codec, NULL) < 0) {
        LOGE("Could not open codec\n");
        exit(1);
    }

    f = fopen(filename, "rb");
    if (!f) {
        LOGE("Could not open %s\n", filename);
        exit(1);
    }
    outfile = fopen(outfilename, "wb");
    if (!outfile) {
        av_free(c);
        exit(1);
    }

    /* decode until eof */
    data      = inbuf;
    data_size = fread(inbuf, 1, AUDIO_INBUF_SIZE, f);

    while (data_size > 0) {
        if (!decoded_frame) {
            if (!(decoded_frame = av_frame_alloc())) {
                LOGE("Could not allocate audio frame\n");
                exit(1);
            }
        }

        ret = av_parser_parse2(parser, c, &pkt->data, &pkt->size,
                               data, data_size,
                               AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (ret < 0) {
            LOGE("Error while parsing\n");
            exit(1);
        }
        data      += ret;
        data_size -= ret;

        if (pkt->size)
            decode_audio(c, pkt, decoded_frame, outfile);

        if (data_size < AUDIO_REFILL_THRESH) {
            memmove(inbuf, data, data_size);
            data = inbuf;
            len = fread(data + data_size, 1,
                        AUDIO_INBUF_SIZE - data_size, f);
            if (len > 0)
                data_size += len;
        }
    }

    /* flush the decoder */
    pkt->data = NULL;
    pkt->size = 0;
    decode_audio(c, pkt, decoded_frame, outfile);

    /* print output pcm infomations, because there have no metadata of pcm */
    sfmt = c->sample_fmt;

    if (av_sample_fmt_is_planar(sfmt)) {
        const char *packed = av_get_sample_fmt_name(sfmt);
        LOGD("Warning: the sample format the decoder produced is planar "
               "(%s). This example will output the first channel only.\n",
               packed ? packed : "?");
        sfmt = av_get_packed_sample_fmt(sfmt);
    }

    n_channels = c->ch_layout.nb_channels;
    if ((ret = get_format_from_sample_fmt(&fmt, sfmt)) < 0)
        goto end;

    LOGD("Play the output audio file with the command:\n"
           "ffplay -f %s -ac %d -ar %d %s\n",
           fmt, n_channels, c->sample_rate,
           outfilename);
    end:
    fclose(outfile);
    fclose(f);

    avcodec_free_context(&c);
    av_parser_close(parser);
    av_frame_free(&decoded_frame);
    av_packet_free(&pkt);

    return 0;
}




