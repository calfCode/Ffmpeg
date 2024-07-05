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
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersrc.h"
#include "libavfilter/buffersink.h"
#include "libavutil/md5.h"
#include "libswresample/swresample.h"
#ifdef __cplusplus
}
#endif

#include <thread>
#include <unistd.h>

//#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG , TAG, __VA_ARGS__)
static AVFormatContext *pFormatCtx = NULL;
static int tryTimes = 0;
#define STREAM_DURATION   10.0
#define STREAM_FRAME_RATE 25 /* 25 images/s */
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */

#define SCALE_FLAGS SWS_BICUBIC

#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096


#define INPUT_SAMPLERATE     48000
#define INPUT_FORMAT         AV_SAMPLE_FMT_FLTP
#define INPUT_CHANNEL_LAYOUT (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT0

#define VOLUME_VAL 0.90


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

static AVCodecContext *dec_ctx;
AVFilterContext *buffersink_ctx;
AVFilterContext *buffersrc_ctx;
AVFilterGraph *filter_graph;
static int video_stream_index = -1;
static int audio_stream_index = -1;
static int64_t last_pts = AV_NOPTS_VALUE;

//const char *video_filter_descr = "scale=78:24,transpose=cclock";
const char *video_filter_descr = "movie=/data/user/0/com.smartdevice.ffmpeg/app_video/1.png[logo];[logo]colorkey=red:0.2:0.5[alphawm];[in][alphawm]overlay=20:20[out]";
const char *audio_filter_descr = "aresample=8000,aformat=sample_fmts=s16:channel_layouts=mono";
static const char *player       = "ffplay -f s16le -ar 8000 -ac 1 -";

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

int example_demux(const char *filePath,const char *audioOutputPath,const char *videoOutputPath) {
    int ret = 0;

    src_filename = filePath;
    video_dst_filename = videoOutputPath; // 视频流保存路径
    audio_dst_filename = audioOutputPath;// 音频流保存路径

    /* open input file, and allocate format context */
    // 打开文件，分配format上下文
    if (avformat_open_input(&fmt_ctx, src_filename, NULL, NULL) < 0) {
        LOGE("Could not open source file %s\n", src_filename);
        return 1;
    }

    /* retrieve stream information */
    // 获取文件的流信息
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        LOGE("Could not find stream information\n");
        return 1;
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

int example_decode(const char *filePath,const char *audioOutputPath,const char *videoOutputPath)
{
    int ret = 0;


    src_filename = filePath;
    video_dst_filename = videoOutputPath; // 视频流保存路径
    audio_dst_filename = audioOutputPath;// 音频流保存路径

    /* open input file, and allocate format context */
    // 关联fmt_ctx到src_filename文件
    if (avformat_open_input(&fmt_ctx, src_filename, NULL, NULL) < 0) {
        LOGE("Could not open source file %s\n", src_filename);
        return 1;
    }

    /* retrieve stream information */
    // 获取媒体 流信息
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        LOGE("Could not find stream information\n");
        return 1;
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
        return ;
    }

    /* read all the available output packets (in general there may be any
     * number of them */
    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx, pkt);// 从编码器接收编码后的数据
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            LOGE("Error encoding audio frame\n");
            return ;
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
        return 1;
    }

    c = avcodec_alloc_context3(codec); // 分配codec context
    if (!c) {
        LOGE("Could not allocate audio codec context\n");
        return 1;
    }

    /* put sample parameters */
    // 设置编码参数
    c->bit_rate = 64000;

    /* check that the encoder supports s16 pcm input */
    c->sample_fmt = AV_SAMPLE_FMT_S16;
    if (!check_sample_fmt(codec, c->sample_fmt)) {
        LOGE("Encoder does not support sample format %s",
                av_get_sample_fmt_name(c->sample_fmt));
        return 1;
    }

    /* select other audio parameters supported by the encoder */
    c->sample_rate    = select_sample_rate(codec);
    ret = select_channel_layout(codec, &c->ch_layout);
    if (ret < 0)
        return 1;

    /* open it */
    // 打开编码器
    if (avcodec_open2(c, codec, NULL) < 0) {
        LOGE("Could not open codec\n");
        return 1;
    }

    // 打开文件
    f = fopen(filename, "wb");
    if (!f) {
        LOGE("Could not open %s\n", filename);
        return 1;
    }

    /* packet for holding encoded output */
    pkt = av_packet_alloc();
    if (!pkt) {
        LOGE("could not allocate the packet\n");
        return 1;
    }

    /* frame containing input raw audio */
    // 设置frame 参数
    frame = av_frame_alloc();
    if (!frame) {
        LOGE("Could not allocate audio frame\n");
        return 1;
    }
    frame->nb_samples     = c->frame_size;
    frame->format         = c->sample_fmt;
    ret = av_channel_layout_copy(&frame->ch_layout, &c->ch_layout);// 从编码器上下文拷贝通道布局
    if (ret < 0)
        return 1;

    /* allocate the data buffers */
    // 分配内存
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        LOGE("Could not allocate audio data buffers\n");
        return 1;
    }

    /* encode a single tone sound */
    t = 0;
    tincr = 2 * M_PI * 440.0 / c->sample_rate;
    for (i = 0; i < 200; i++) {
        /* make sure the frame is writable -- makes a copy if the encoder
         * kept a reference internally */
        ret = av_frame_make_writable(frame); // 设置frame 为可写入的
        if (ret < 0)
            return 1;
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
        return ;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(enc_ctx, pkt);// 获取编码后的数据
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            LOGE("Error during encoding\n");
            return ;
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
        return 1;
    }

    c = avcodec_alloc_context3(codec);// 分配codec上下文
    if (!c) {
        LOGE("Could not allocate video codec context\n");
        return 1;
    }

    pkt = av_packet_alloc(); // 分配packet
    if (!pkt)
        return 1;

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
        return 1;
    }

    f = fopen(filename, "wb");
    if (!f) {
        LOGE("Could not open %s\n", filename);
        return 1;
    }

    frame = av_frame_alloc();
    if (!frame) {
        LOGE("Could not allocate video frame\n");
        return 1;
    }
    frame->format = c->pix_fmt;
    frame->width  = c->width;
    frame->height = c->height;
    // 分配frame 的数据空间
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        LOGE("Could not allocate the video frame data\n");
        return 1;
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
            return 1;

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
        return ;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame); // 接收解码器的输出数据
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            LOGE("Error during decoding\n");
            return ;
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

int example_video_decode(const char *filePath,const char *outputPath)
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
    outfilename = outputPath;

    pkt = av_packet_alloc();
    if (!pkt)
        return 1;

    /* set end of buffer to 0 (this ensures that no overreading happens for damaged MPEG streams) */
    memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    /* find the MPEG-1 video decoder */
    codec = avcodec_find_decoder(AV_CODEC_ID_MPEG1VIDEO); // 查找解码器
    if (!codec) {
        LOGE("Codec not found\n");
        return 1;
    }

    parser = av_parser_init(codec->id);// 根据codec id初始化AVCodecParserContext
    if (!parser) {
        LOGE("parser not found\n");
        return 1;
    }

    c = avcodec_alloc_context3(codec);// 分配AVCodecContext
    if (!c) {
        LOGE("Could not allocate video codec context\n");
        return 1;
    }

    /* For some codecs, such as msmpeg4 and mpeg4, width and height
       MUST be initialized there because this information is not
       available in the bitstream. */

    /* open it */
    if (avcodec_open2(c, codec, NULL) < 0) { // 打开解码器
        LOGE("Could not open codec\n");
        return 1;
    }

    f = fopen(filename, "rb");
    if (!f) {
        LOGE("Could not open %s\n", filename);
        return 1;
    }

    frame = av_frame_alloc();
    if (!frame) {
        LOGE("Could not allocate video frame\n");
        return 1;
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
            // 用来在解码的时候解析和读取完整的压缩帧数据，它会使用内部缓存收集数据包，知道它找到足够的数据组成一个完整的帧，然后这个帧被发送到解码器进行解码
            ret = av_parser_parse2(parser, c, &pkt->data, &pkt->size,
                                   data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (ret < 0) {
                LOGE("Error while parsing\n");
                return 1;
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
        return ;
    }

    /* read all the output frames (in general there may be any number of them */
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            LOGE("Error during decoding\n");
            return ;
        }
        data_size = av_get_bytes_per_sample(dec_ctx->sample_fmt);
        if (data_size < 0) {
            /* This should not occur, checking just for paranoia */
            LOGE("Failed to calculate data size\n");
            return ;
        }
        for (i = 0; i < frame->nb_samples; i++)
            for (ch = 0; ch < dec_ctx->ch_layout.nb_channels; ch++)
                fwrite(frame->data[ch] + data_size*i, 1, data_size, outfile);
    }
}

int example_audio_decode(const char *filePath,const char *outputPath)
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
    outfilename = outputPath;
    pkt = av_packet_alloc();

    /* find the MPEG audio decoder */
    codec = avcodec_find_decoder(AV_CODEC_ID_MP2);// 查找解码器
    if (!codec) {
        LOGE("Codec not found\n");
        return 1;
    }

    parser = av_parser_init(codec->id);
    if (!parser) {
        LOGE("Parser not found\n");
        return 1;
    }

    c = avcodec_alloc_context3(codec);
    if (!c) {
        LOGE("Could not allocate audio codec context\n");
        return 1;
    }

    /* open it */
     // 打开解码器
    if (avcodec_open2(c, codec, NULL) < 0) {
        LOGE("Could not open codec\n");
        return 1;
    }

    f = fopen(filename, "rb");
    if (!f) {
        LOGE("Could not open %s\n", filename);
        return 1;
    }
    outfile = fopen(outfilename, "wb");
    if (!outfile) {
        av_free(c);
        return 1;
    }

    /* decode until eof */
    data      = inbuf;
    data_size = fread(inbuf, 1, AUDIO_INBUF_SIZE, f);

    while (data_size > 0) {
        if (!decoded_frame) {
            if (!(decoded_frame = av_frame_alloc())) {
                LOGE("Could not allocate audio frame\n");
                return 1;
            }
        }
// 将音频数据解析出来
        ret = av_parser_parse2(parser, c, &pkt->data, &pkt->size,
                               data, data_size,
                               AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (ret < 0) {
            LOGE("Error while parsing\n");
            return 1;
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


static int open_video_input_file(const char *filename)
{
    const AVCodec *dec;
    int ret;
    // 关联AVFormatContext到输入文件
    if ((ret = avformat_open_input(&fmt_ctx, filename, NULL, NULL)) < 0) {
        LOGE("Cannot open input file\n");
        return ret;
    }
    // 查找文件中的流
    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
        LOGE("Cannot find stream information\n");
        return ret;
    }
    // 选择最优的视频流
    /* select the video stream */
    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (ret < 0) {
        LOGE("Cannot find a video stream in the input file\n");
        return ret;
    }
    video_stream_index = ret;

    /* create decoding context */
    // 创建avcodec context
    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx)
        return AVERROR(ENOMEM);

    // 从流拷贝codec参数到avcodec context
    avcodec_parameters_to_context(dec_ctx, fmt_ctx->streams[video_stream_index]->codecpar);

    /* init the video decoder */
    // 打开视频解码器
    if ((ret = avcodec_open2(dec_ctx, dec, NULL)) < 0) {
        LOGE("Cannot open video decoder\n");
        return ret;
    }

    return 0;
}

static int init_video_filters(const char *filters_descr)
{
    char args[512];
    int ret = 0;
    const AVFilter *buffersrc  = avfilter_get_by_name("buffer");// 获取buffer  源滤镜
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");// 获取buffersink  sink滤镜
    AVFilterInOut *outputs = avfilter_inout_alloc(); // 申请输入的滤镜结构
    AVFilterInOut *inputs  = avfilter_inout_alloc(); // 申请输出的滤镜结构
    AVRational time_base = fmt_ctx->streams[video_stream_index]->time_base;
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE };

    filter_graph = avfilter_graph_alloc(); // 申请AVFilterGraph，用于存储滤镜的in和out描述信息
    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    // 创建输入的AVFilterContext
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
             time_base.num, time_base.den,
             dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                       args, NULL, filter_graph);
    if (ret < 0) {
        LOGE("Cannot create buffer source\n");
        goto end;
    }

    /* buffer video sink: to terminate the filter chain. */
    // 创建输出的AVFilterContext
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                       NULL, NULL, filter_graph);
    if (ret < 0) {
        LOGE("Cannot create buffer sink\n");
        goto end;
    }

    ret = av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts,
                              AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        LOGE("Cannot set output pixel format\n");
        goto end;
    }

    /*
     * Set the endpoints for the filter graph. The filter_graph will
     * be linked to the graph described by filters_descr.
     */

    /*
     * The buffer source output must be connected to the input pad of
     * the first filter described by filters_descr; since the first
     * filter input label is not specified, it is set to "in" by
     * default.
     */
     // 创建滤镜解析器
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    /*
     * The buffer sink input must be connected to the output pad of
     * the last filter described by filters_descr; since the last
     * filter output label is not specified, it is set to "out" by
     * default.
     */
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr,
                                        &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;

    end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

static void display_frame(const AVFrame *frame, AVRational time_base)
{
    int x, y;
    uint8_t *p0, *p;
    int64_t delay;

    if (frame->pts != AV_NOPTS_VALUE) {
        if (last_pts != AV_NOPTS_VALUE) {
            /* sleep roughly the right amount of time;
             * usleep is in microseconds, just like AV_TIME_BASE. */
            delay = av_rescale_q(frame->pts - last_pts,
                                 time_base, AV_TIME_BASE_Q);
            if (delay > 0 && delay < 1000000)
                usleep(delay);
        }
        last_pts = frame->pts;
    }

    /* Trivial ASCII grayscale display. */
    p0 = frame->data[0];
    puts("\033c");
    for (y = 0; y < frame->height; y++) {
        p = p0;
        for (x = 0; x < frame->width; x++)
            putchar(" .-+#"[*(p++) / 52]);
        putchar('\n');
        p0 += frame->linesize[0];
    }
    fflush(stdout);
}

int example_video_decode_filter(const char *filePath,const char *outputPath)
{
    int ret;
    AVPacket *packet;
    AVFrame *frame;
    AVFrame *filt_frame;

    char buf[1024];
    frame = av_frame_alloc();
    filt_frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!frame || !filt_frame || !packet) {
        LOGE("Could not allocate frame or packet\n");
        return 0;
    }
    const char* outFilePath = outputPath;

    // 根据输入文件打开视频解码器
    if ((ret = open_video_input_file(filePath)) < 0){
        LOGE("open_video_input_file fail\n");
        goto end;
    }

    // 初始化视频滤镜
    if ((ret = init_video_filters(video_filter_descr)) < 0){
        LOGE("init_video_filters fail\n");
        goto end;
    }

    /* read all packets */
    while (1) {
        if ((ret = av_read_frame(fmt_ctx, packet)) < 0)
            break;

        if (packet->stream_index == video_stream_index) {
            ret = avcodec_send_packet(dec_ctx, packet);
            if (ret < 0) {
                LOGE("Error while sending a packet to the decoder\n");
                break;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(dec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    LOGE("Error while receiving a frame from the decoder\n");
                    goto end;
                }

                frame->pts = frame->best_effort_timestamp;

                /* push the decoded frame into the filtergraph */
                // 将解码后的视频帧抛给AVFilterContext进行处理
                if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                    LOGE("Error while feeding the filtergraph\n");
                    break;
                }

                /* pull filtered frames from the filtergraph */
                // 获取滤镜处理后的数据
                while (1) {
                    ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                    if (ret < 0)
                        goto end;
                    display_frame(filt_frame, buffersink_ctx->inputs[0]->time_base);
                    snprintf(buf, sizeof(buf), "%s-%" PRId64, outFilePath, dec_ctx->frame_num);
                    pgm_save(filt_frame->data[0], filt_frame->linesize[0],
                             filt_frame->width, filt_frame->height, buf);// 保存文件
                    av_frame_unref(filt_frame);
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
    }
    end:
    avfilter_graph_free(&filter_graph);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    av_frame_free(&frame);
    av_frame_free(&filt_frame);
    av_packet_free(&packet);

    if (ret < 0 && ret != AVERROR_EOF) {
        LOGE("Error occurred: %s\n", av_err2str(ret));
        return 1;
    }

    return 0;
}

static int open_audio_input_file(const char *filename)
{
    const AVCodec *dec;
    int ret;

    if ((ret = avformat_open_input(&fmt_ctx, filename, NULL, NULL)) < 0) {
       LOGE("Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
       LOGE("Cannot find stream information\n");
        return ret;
    }

    /* select the audio stream */
    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
    if (ret < 0) {
       LOGE("Cannot find an audio stream in the input file\n");
        return ret;
    }
    audio_stream_index = ret;

    /* create decoding context */
    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx)
        return AVERROR(ENOMEM);
    avcodec_parameters_to_context(dec_ctx, fmt_ctx->streams[audio_stream_index]->codecpar);

    /* init the audio decoder */
    if ((ret = avcodec_open2(dec_ctx, dec, NULL)) < 0) {
       LOGE("Cannot open audio decoder\n");
        return ret;
    }

    return 0;
}

static int init_audio_filters(const char *filters_descr)
{
    char args[512];
    int ret = 0;
    const AVFilter *abuffersrc  = avfilter_get_by_name("abuffer");
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    static const enum AVSampleFormat out_sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
    static const int out_sample_rates[] = { 8000, -1 };
    const AVFilterLink *outlink;
    AVRational time_base = fmt_ctx->streams[audio_stream_index]->time_base;

    filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer audio source: the decoded frames from the decoder will be inserted here. */
    if (dec_ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&dec_ctx->ch_layout, dec_ctx->ch_layout.nb_channels);
    ret = snprintf(args, sizeof(args),
                   "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=",
                   time_base.num, time_base.den, dec_ctx->sample_rate,
                   av_get_sample_fmt_name(dec_ctx->sample_fmt));
    av_channel_layout_describe(&dec_ctx->ch_layout, args + ret, sizeof(args) - ret);
    ret = avfilter_graph_create_filter(&buffersrc_ctx, abuffersrc, "in",
                                       args, NULL, filter_graph);
    if (ret < 0) {
       LOGE("Cannot create audio buffer source\n");
        goto end;
    }

    /* buffer audio sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&buffersink_ctx, abuffersink, "out",
                                       NULL, NULL, filter_graph);
    if (ret < 0) {
       LOGE("Cannot create audio buffer sink\n");
        goto end;
    }

    ret = av_opt_set_int_list(buffersink_ctx, "sample_fmts", out_sample_fmts, -1,
                              AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
       LOGE("Cannot set output sample format\n");
        goto end;
    }

    ret = av_opt_set(buffersink_ctx, "ch_layouts", "mono",
                     AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
       LOGE("Cannot set output channel layout\n");
        goto end;
    }

    ret = av_opt_set_int_list(buffersink_ctx, "sample_rates", out_sample_rates, -1,
                              AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
       LOGE("Cannot set output sample rate\n");
        goto end;
    }

    /*
     * Set the endpoints for the filter graph. The filter_graph will
     * be linked to the graph described by filters_descr.
     */

    /*
     * The buffer source output must be connected to the input pad of
     * the first filter described by filters_descr; since the first
     * filter input label is not specified, it is set to "in" by
     * default.
     */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    /*
     * The buffer sink input must be connected to the output pad of
     * the last filter described by filters_descr; since the last
     * filter output label is not specified, it is set to "out" by
     * default.
     */
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr,
                                        &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;

    /* Print summary of the sink buffer
     * Note: args buffer is reused to store channel layout string */
    outlink = buffersink_ctx->inputs[0];
    av_channel_layout_describe(&outlink->ch_layout, args, sizeof(args));
    av_log(NULL, AV_LOG_INFO, "Output: srate:%dHz fmt:%s chlayout:%s\n",
           (int)outlink->sample_rate,
           (char *)av_x_if_null(av_get_sample_fmt_name((AVSampleFormat)outlink->format), "?"),
           args);

    end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

static void print_frame(const AVFrame *frame)
{
    const int n = frame->nb_samples * frame->ch_layout.nb_channels;
    const uint16_t *p     = (uint16_t*)frame->data[0];
    const uint16_t *p_end = p + n;
    LOGD("print_frame n=%d",n);
    LOGD("print_frame frame->ch_layout.nb_channels=%d",frame->ch_layout.nb_channels);
    LOGD("print_frame frame->nb_samples=%d",frame->nb_samples);
    while (p < p_end) {
        fputc(*p    & 0xff, stdout);
        fputc(*p>>8 & 0xff, stdout);
        p++;
    }
    fflush(stdout);
}

int example_audio_decode_filter(const char *filePath,const char *outputPath)
{
    const char *outfilename = outputPath;
    FILE *outfile = fopen(outfilename, "wb");

    int ret;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *filt_frame = av_frame_alloc();
    size_t   data_size;
    int i=0;
    data_size = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
    if (!packet || !frame || !filt_frame) {
        LOGE("Could not allocate frame or packet\n");
        return 1;
    }

    if ((ret = open_audio_input_file(filePath)) < 0){
        LOGE("open_audio_input_file fail\n");
        goto end;
    }

    if ((ret = init_audio_filters(audio_filter_descr)) < 0){
        LOGE("init_audio_filters fail\n");
        goto end;
    }

    /* read all packets */
    while (1) {
        if ((ret = av_read_frame(fmt_ctx, packet)) < 0)
            break;

        if (packet->stream_index == audio_stream_index) {
            ret = avcodec_send_packet(dec_ctx, packet);
            if (ret < 0) {
               LOGE("Error while sending a packet to the decoder\n");
                break;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(dec_ctx, frame);
                LOGD(" avcodec_receive_frame ret=%d",ret);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                   LOGE("Error while receiving a frame from the decoder\n");
                    goto end;
                }

                if (ret >= 0) {
                    /* push the audio data from decoded frame into the filtergraph */
                    if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                       LOGE("Error while feeding the audio filtergraph\n");
                        break;
                    }

                    /* pull filtered audio from the filtergraph */
                    while (1) {
                        ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
                        LOGD(" av_buffersink_get_frame ret=%d",ret);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                            break;
                        if (ret < 0)
                            goto end;
                        print_frame(filt_frame);

                        for (i = 0; i < filt_frame->nb_samples; i++){
                            fwrite(filt_frame->data[0] + data_size*i, 1, data_size, outfile);
                        }


                        av_frame_unref(filt_frame);
                    }
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(packet);
    }
    end:
    fclose(outfile);
    avfilter_graph_free(&filter_graph);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&filt_frame);

    if (ret < 0 && ret != AVERROR_EOF) {
        LOGE("Error occurred: %s\n", av_err2str(ret));
        return 1;
    }

    return 0;
}


static int init_audio_filter_graph(AVFilterGraph **graph, AVFilterContext **src,
                             AVFilterContext **sink)
{
    AVFilterGraph *filter_graph;
    AVFilterContext *abuffer_ctx;
    const AVFilter  *abuffer;
    AVFilterContext *volume_ctx;
    const AVFilter  *volume;
    AVFilterContext *aformat_ctx;
    const AVFilter  *aformat;
    AVFilterContext *abuffersink_ctx;
    const AVFilter  *abuffersink;

    AVDictionary *options_dict = NULL;
    char options_str[1024];
    char ch_layout[64];

    int err;

    /* Create a new filtergraph, which will contain all the filters. */
    filter_graph = avfilter_graph_alloc();// 创建AVFilterGraph
    if (!filter_graph) {
        LOGE("Unable to create filter graph.\n");
        return AVERROR(ENOMEM);
    }

    /* Create the abuffer filter;
     * it will be used for feeding the data into the graph. */
    abuffer = avfilter_get_by_name("abuffer");// 获取abuffer filter
    if (!abuffer) {
        LOGE("Could not find the abuffer filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }

    abuffer_ctx = avfilter_graph_alloc_filter(filter_graph, abuffer, "src");// 获取abuffer filter对应的AVFilterContext
    if (!abuffer_ctx) {
        LOGE("Could not allocate the abuffer instance.\n");
        return AVERROR(ENOMEM);
    }

    /* Set the filter options through the AVOptions API. */
    const AVChannelLayout avChannelLayout = INPUT_CHANNEL_LAYOUT;
    // 获取channel layout的描述
    av_channel_layout_describe(&avChannelLayout, ch_layout, sizeof(ch_layout));
    // 根据获取的描述设置abuffer filter的AVFilterContext的channel_layout参数
    av_opt_set(abuffer_ctx, "channel_layout", ch_layout,                            AV_OPT_SEARCH_CHILDREN);
    // 设置abuffer filter的AVFilterContext的sample_fmt参数
    av_opt_set(abuffer_ctx, "sample_fmt",     av_get_sample_fmt_name(INPUT_FORMAT), AV_OPT_SEARCH_CHILDREN);
    // 设置abuffer filter的AVFilterContext的time_base参数
    av_opt_set_q(abuffer_ctx, "time_base",      (AVRational){ 1, INPUT_SAMPLERATE },  AV_OPT_SEARCH_CHILDREN);
    // 设置abuffer filter的AVFilterContext的sample_rate参数
    av_opt_set_int(abuffer_ctx, "sample_rate",    INPUT_SAMPLERATE,                     AV_OPT_SEARCH_CHILDREN);

    /* Now initialize the filter; we pass NULL options, since we have already
     * set all the options above. */
    // 以字符方式初始化abuffe filter
    err = avfilter_init_str(abuffer_ctx, NULL);
    if (err < 0) {
        LOGE("Could not initialize the abuffer filter.\n");
        return err;
    }

    /* Create volume filter. */
    // 创建声音滤镜
    volume = avfilter_get_by_name("volume");
    if (!volume) {
        LOGE("Could not find the volume filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }
    // 创建声音滤镜的上下文
    volume_ctx = avfilter_graph_alloc_filter(filter_graph, volume, "volume");
    if (!volume_ctx) {
        LOGE("Could not allocate the volume instance.\n");
        return AVERROR(ENOMEM);
    }

    /* A different way of passing the options is as key/value pairs in a
     * dictionary. */
    // 设置字典的volume 参数
    av_dict_set(&options_dict, "volume", AV_STRINGIFY(VOLUME_VAL), 0);
     // 以字典方式初始化声音滤镜
    err = avfilter_init_dict(volume_ctx, &options_dict);
    // 释放字典
    av_dict_free(&options_dict);
    if (err < 0) {
        LOGE("Could not initialize the volume filter.\n");
        return err;
    }

    /* Create the aformat filter;
     * it ensures that the output is of the format we want. */
    // 创建aformat 滤镜
    aformat = avfilter_get_by_name("aformat");
    if (!aformat) {
        LOGE("Could not find the aformat filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }
    // 创建aformat 滤镜的上下文
    aformat_ctx = avfilter_graph_alloc_filter(filter_graph, aformat, "aformat");
    if (!aformat_ctx) {
        LOGE("Could not allocate the aformat instance.\n");
        return AVERROR(ENOMEM);
    }

    /* A third way of passing the options is in a string of the form
     * key1=value1:key2=value2.... */
    // 转为AV_SAMPLE_FMT_S16采样格式、比特率44100 立体声
    snprintf(options_str, sizeof(options_str),
             "sample_fmts=%s:sample_rates=%d:channel_layouts=stereo",
             av_get_sample_fmt_name(AV_SAMPLE_FMT_S16), 44100);
    // 以字符参数方式初始化滤镜
    err = avfilter_init_str(aformat_ctx, (const char*)options_str);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not initialize the aformat filter.\n");
        return err;
    }

    /* Finally create the abuffersink filter;
     * it will be used to get the filtered data out of the graph. */
    // 创建abuffersink滤镜
    abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffersink) {
        LOGE("Could not find the abuffersink filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }
    // 创建abuffersink滤镜的上下文
    abuffersink_ctx = avfilter_graph_alloc_filter(filter_graph, abuffersink, "sink");
    if (!abuffersink_ctx) {
        LOGE("Could not allocate the abuffersink instance.\n");
        return AVERROR(ENOMEM);
    }

    /* This filter takes no options. */
    // 以字符参数方式初始化滤镜
    err = avfilter_init_str(abuffersink_ctx, NULL);
    if (err < 0) {
        LOGE("Could not initialize the abuffersink instance.\n");
        return err;
    }

    /* Connect the filters;
     * in this simple case the filters just form a linear chain. */
    // 连接abuffer和volume 滤镜
    err = avfilter_link(abuffer_ctx, 0, volume_ctx, 0);
    if (err >= 0)
        err = avfilter_link(volume_ctx, 0, aformat_ctx, 0);  // 连接volume和 aformat滤镜
    if (err >= 0)
        err = avfilter_link(aformat_ctx, 0, abuffersink_ctx, 0);// 连接aformat和abuffersink 滤镜
    if (err < 0) {
        LOGE("Error connecting filters\n");
        return err;
    }

    /* Configure the graph. */
    // 配置滤镜图
    err = avfilter_graph_config(filter_graph, NULL);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error configuring the filter graph\n");
        return err;
    }

    *graph = filter_graph;
    *src   = abuffer_ctx;
    *sink  = abuffersink_ctx;

    return 0;
}

/* Do something useful with the filtered data: this simple
 * example just prints the MD5 checksum of each plane to stdout. */
static int process_audio_output(struct AVMD5 *md5, AVFrame *frame)
{
    int planar     = av_sample_fmt_is_planar((AVSampleFormat)frame->format);
    int channels   = frame->ch_layout.nb_channels;
    int planes     = planar ? channels : 1;
    int bps        = av_get_bytes_per_sample((AVSampleFormat)frame->format);
    int plane_size = bps * frame->nb_samples * (planar ? 1 : channels);
    int i, j;
//    LOGD("process_audio_output planar=%d,channels=%d,bps=%d",planar,channels,bps);

    for (i = 0; i < planes; i++) {
        uint8_t checksum[16];

        av_md5_init(md5);
        av_md5_sum(checksum, frame->extended_data[i], plane_size);

        fprintf(stdout, "plane %d: 0x", i);
        for (j = 0; j < sizeof(checksum); j++)
            fprintf(stdout, "%02X", checksum[j]);
        fprintf(stdout, "\n");
    }
    fprintf(stdout, "\n");

    return 0;
}
static int save_audio(AVFrame *frame,FILE *outfile){
    int data_size =   av_get_bytes_per_sample((AVSampleFormat)frame->format);
    int nb_samples = frame->nb_samples;
    int planar     = av_sample_fmt_is_planar((AVSampleFormat)frame->format);
    int channels   = frame->ch_layout.nb_channels;
    int planes     = planar ? channels : 1;
    LOGD("save_audio data_size=%d,channels=%d,nb_samples=%d",data_size,channels,nb_samples);
    for (int i = 0; i < nb_samples; i++){
        LOGD("save_audio i=%d",i);
        for (int j = 0; j < planes; j++) {
//            LOGD("save_audio fwrite j=%d ,data_size=%d",j,data_size);
            fwrite(frame->data[j] + data_size*i, 1, data_size, outfile);
        }
    }

    return 0;
}

/* Construct a frame of audio data to be filtered;
 * this simple example just synthesizes a sine wave. */
static int get_audio_input(AVFrame *frame, int frame_num)
{
    int err, i, j;

#define FRAME_SIZE 1024
    const AVChannelLayout src =  INPUT_CHANNEL_LAYOUT;
    /* Set up the frame properties and allocate the buffer for the data. */
    frame->sample_rate    = INPUT_SAMPLERATE;
    frame->format         = INPUT_FORMAT;
    av_channel_layout_copy(&frame->ch_layout, &src);
    frame->nb_samples     = FRAME_SIZE;
    frame->pts            = frame_num * FRAME_SIZE;

    err = av_frame_get_buffer(frame, 0);// 分配的frame的数据存储空间
    if (err < 0)
        return err;

    /* Fill the data for each channel. */
    // 填充frame的数据
    for (i = 0; i < 5; i++) {
        float *data = (float*)frame->extended_data[i];

        for (j = 0; j < frame->nb_samples; j++)
            data[j] = sin(2 * M_PI * (frame_num + j) * (i + 1) / FRAME_SIZE);
    }

    return 0;
}

int example_filter_audio(int time,const char* outputPath)
{
    struct AVMD5 *md5;
    AVFilterGraph *graph;
    AVFilterContext *src, *sink;
    AVFrame *frame;
    char errstr[1024];
    float duration;
    int err, nb_frames, i;

    const char *outfilename = outputPath;
    FILE *outfile = fopen(outfilename, "wb");
    duration  = float(time);
    nb_frames = duration * INPUT_SAMPLERATE / FRAME_SIZE;
    if (nb_frames <= 0) {
        LOGE("Invalid duration: %d\n", time);
        return 1;
    }

    /* Allocate the frame we will be using to store the data. */
    frame  = av_frame_alloc();
    if (!frame) {
        LOGE("Error allocating the frame\n");
        return 1;
    }

    md5 = av_md5_alloc();
    if (!md5) {
        LOGE("Error allocating the MD5 context\n");
        return 1;
    }

    /* Set up the filtergraph. */
    err = init_audio_filter_graph(&graph, &src, &sink);
    if (err < 0) {
        LOGE("Unable to init filter graph:");
        goto fail;
    }

    /* the main filtering loop */
    for (i = 0; i < nb_frames; i++) {
        /* get an input frame to be filtered */
        err = get_audio_input(frame, i);
        if (err < 0) {
            LOGE("Error generating input frame:");
            goto fail;
        }

        /* Send the frame to the input of the filtergraph. */
        // 发送数据给滤镜图的输入
        err = av_buffersrc_add_frame(src, frame);
        if (err < 0) {
            av_frame_unref(frame);
            LOGE("Error submitting the frame to the filtergraph:");
            goto fail;
        }

        /* Get all the filtered output that is available. */
        //从滤镜图的输出接收数据
        while ((err = av_buffersink_get_frame(sink, frame)) >= 0) {
            /* now do something with our filtered frame */
//            err = process_audio_output(md5, frame);
//            if (err < 0) {
//                LOGE("Error processing the filtered frame:");
//                goto fail;
//            }
            save_audio(frame,outfile);
            av_frame_unref(frame);
        }

        if (err == AVERROR(EAGAIN)) {
            /* Need to feed more frames in. */
            continue;
        } else if (err == AVERROR_EOF) {
            /* Nothing more to do, finish. */
            break;
        } else if (err < 0) {
            /* An error occurred. */
            LOGE("Error filtering the data:");
            goto fail;
        }
    }

    avfilter_graph_free(&graph);
    av_frame_free(&frame);
    av_freep(&md5);

    return 0;

    fail:
    av_strerror(err, errstr, sizeof(errstr));
    fclose(outfile);
    LOGE("%s\n", errstr);
    return 1;
}



/**
 * Fill dst buffer with nb_samples, generated starting from t.
 */
static void fill_samples(double *dst, int nb_samples, int nb_channels, int sample_rate, double *t)
{
    int i, j;
    double tincr = 1.0 / sample_rate, *dstp = dst;
    const double c = 2 * M_PI * 440.0;

    /* generate sin tone with 440Hz frequency and duplicated channels */
    for (i = 0; i < nb_samples; i++) {
        *dstp = sin(c * *t);
        for (j = 1; j < nb_channels; j++)
            dstp[j] = dstp[0];
        dstp += nb_channels;
        *t += tincr;
    }
}

int example_resample_audio(const char* outputPath)
{
    AVChannelLayout src_ch_layout = AV_CHANNEL_LAYOUT_STEREO, dst_ch_layout = AV_CHANNEL_LAYOUT_SURROUND;
    int src_rate = 48000, dst_rate = 44100;
    uint8_t **src_data = NULL, **dst_data = NULL;
    int src_nb_channels = 0, dst_nb_channels = 0;
    int src_linesize, dst_linesize;
    int src_nb_samples = 1024, dst_nb_samples, max_dst_nb_samples;
    enum AVSampleFormat src_sample_fmt = AV_SAMPLE_FMT_DBL, dst_sample_fmt = AV_SAMPLE_FMT_S16;
    const char *dst_filename = NULL;
    FILE *dst_file;
    int dst_bufsize;
    const char *fmt;
    struct SwrContext *swr_ctx;
    char buf[64];
    double t;
    int ret;


    dst_filename = outputPath;

    dst_file = fopen(dst_filename, "wb+");
    if (!dst_file) {
        LOGE("Could not open destination file %s\n", dst_filename);
        return 1;
    }

    /* create resampler context */
    // 创建重采样上下文
    swr_ctx = swr_alloc();
    if (!swr_ctx) {
        LOGE("Could not allocate resampler context\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* set options */
    av_opt_set_chlayout(swr_ctx, "in_chlayout",    &src_ch_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate",       src_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", src_sample_fmt, 0);

    av_opt_set_chlayout(swr_ctx, "out_chlayout",    &dst_ch_layout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate",       dst_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", dst_sample_fmt, 0);

    /* initialize the resampling context */
    // 初始化重采样上下文
    if ((ret = swr_init(swr_ctx)) < 0) {
        LOGE("Failed to initialize the resampling context\n");
        goto end;
    }

    /* allocate source and destination samples buffers */
    // 分配源采样 缓存空间
    src_nb_channels = src_ch_layout.nb_channels;
    ret = av_samples_alloc_array_and_samples(&src_data, &src_linesize, src_nb_channels,
                                             src_nb_samples, src_sample_fmt, 0);
    if (ret < 0) {
        LOGE("Could not allocate source samples\n");
        goto end;
    }
    // 分配 目标采样缓存空间
    /* compute the number of converted samples: buffering is avoided
     * ensuring that the output buffer will contain at least all the
     * converted input samples */
    max_dst_nb_samples = dst_nb_samples =
            av_rescale_rnd(src_nb_samples, dst_rate, src_rate, AV_ROUND_UP);

    /* buffer is going to be directly written to a rawaudio file, no alignment */
    dst_nb_channels = dst_ch_layout.nb_channels;
    ret = av_samples_alloc_array_and_samples(&dst_data, &dst_linesize, dst_nb_channels,
                                             dst_nb_samples, dst_sample_fmt, 0);
    if (ret < 0) {
        LOGE("Could not allocate destination samples\n");
        goto end;
    }

    t = 0;
    do {
        /* generate synthetic audio */
        //  产生模拟声音信号 填充到源采样 缓存空间
        fill_samples((double *)src_data[0], src_nb_samples, src_nb_channels, src_rate, &t);

        /* compute destination number of samples */
        // 计算目标采样数
        dst_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, src_rate) +
                                        src_nb_samples, dst_rate, src_rate, AV_ROUND_UP);
        if (dst_nb_samples > max_dst_nb_samples) {
            av_freep(&dst_data[0]);
            ret = av_samples_alloc(dst_data, &dst_linesize, dst_nb_channels,
                                   dst_nb_samples, dst_sample_fmt, 1);
            if (ret < 0)
                break;
            max_dst_nb_samples = dst_nb_samples;
        }

        /* convert to destination format */
         // 转换到目标格式
        ret = swr_convert(swr_ctx, dst_data, dst_nb_samples, (const uint8_t **)src_data, src_nb_samples);
        if (ret < 0) {
            LOGE("Error while converting\n");
            goto end;
        }
        // 获取转换后的大小
        dst_bufsize = av_samples_get_buffer_size(&dst_linesize, dst_nb_channels,
                                                 ret, dst_sample_fmt, 1);
        if (dst_bufsize < 0) {
            LOGE("Could not get sample buffer size\n");
            goto end;
        }
        printf("t:%f in:%d out:%d\n", t, src_nb_samples, ret);
        // 写入到文件中
        fwrite(dst_data[0], 1, dst_bufsize, dst_file);
    } while (t < 10);

    if ((ret = get_format_from_sample_fmt(&fmt, dst_sample_fmt)) < 0)
        goto end;
    av_channel_layout_describe(&dst_ch_layout, buf, sizeof(buf));
    LOGE("Resampling succeeded. Play the output file with the command:\n"
                    "ffplay -f %s -channel_layout %s -channels %d -ar %d %s\n",
            fmt, buf, dst_nb_channels, dst_rate, dst_filename);

    end:
    fclose(dst_file);

    if (src_data)
        av_freep(&src_data[0]);
    av_freep(&src_data);

    if (dst_data)
        av_freep(&dst_data[0]);
    av_freep(&dst_data);

    swr_free(&swr_ctx);
    return ret < 0;
}