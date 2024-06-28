#include <jni.h>
#include <string>
#include "ffmpeg.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libswresample/swresample.h"
#include "libavutil/timestamp.h"
#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#ifdef __cplusplus
}
#endif

#define STREAM_DURATION   10.0
#define STREAM_FRAME_RATE 25 /* 25 images/s */
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */

#define SCALE_FLAGS SWS_BICUBIC

// a wrapper around a single output AVStream
typedef struct OutputStream {
    AVStream *st; //视频或音频流
    AVCodecContext *enc;//编码配置

    /* pts of the next frame that will be generated */
    int64_t next_pts;//下一帧的PTS，用于视/音频同步
    int samples_count;//声音采样计数

    AVFrame *frame;//  视频/音频帧
    AVFrame *tmp_frame;//临时帧

    AVPacket *tmp_pkt; //临时包

    float t, tincr, tincr2;//用于声音生成

    struct SwsContext *sws_ctx;//视频转换配置
    struct SwrContext *swr_ctx;//声音重采样配置
} OutputStream;

static AVFrame *alloc_frame(enum AVPixelFormat pix_fmt, int width, int height)
{
    AVFrame *frame;
    int ret;

    frame = av_frame_alloc();
    if (!frame)
        return NULL;

    frame->format = pix_fmt;
    frame->width  = width;
    frame->height = height;

    /* allocate the buffers for the frame data */
    //分配帧数据缓冲区
    //第二个参数32用于对齐，如果搞不清楚怎么设的话，直接设为0就行，FFmpeg会自动处理
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        LOGE("Could not allocate frame data.\n");
        exit(1);
    }

    return frame;
}
static void open_video(AVFormatContext *oc, const AVCodec *codec,
                       OutputStream *ost, AVDictionary *opt_arg)
{
    int ret;
    AVCodecContext *c = ost->enc;
    AVDictionary *opt = NULL;
    //拷贝用户设定的参数字典
    av_dict_copy(&opt, opt_arg, 0);

    /* open the codec */
    //打开编码器，随后释放参数字典
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        LOGE("Could not open video codec: %s\n", av_err2str(ret));
        exit(1);
    }

    /* allocate and init a re-usable frame */
    //分配并初始化一个可重复使用的视频帧，指定好像素点格式和宽高
    ost->frame = alloc_frame(c->pix_fmt, c->width, c->height);
    if (!ost->frame) {
        LOGE("Could not allocate video frame\n");
        exit(1);
    }

    /* If the output format is not YUV420P, then a temporary YUV420P
     * picture is needed too. It is then converted to the required
     * output format. */
    //如果输出格式不是YUV420P，那么需要一个临时的YUV420P帧便于进行转换
    ost->tmp_frame = NULL;
    if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
        ost->tmp_frame = alloc_frame(AV_PIX_FMT_YUV420P, c->width, c->height);
        if (!ost->tmp_frame) {
            LOGE("Could not allocate temporary video frame\n");
            exit(1);
        }
    }

    /* copy the stream parameters to the muxer */
    //从CodecContext中拷贝参数到流/复用器
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        LOGE("Could not copy the stream parameters\n");
        exit(1);
    }

    LOGD("open_video codec->name=%s",codec->name);
}
static AVFrame *alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                  const AVChannelLayout *channel_layout,
                                  int sample_rate, int nb_samples)
{
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        LOGE("Error allocating an audio frame\n");
        exit(1);
    }

    frame->format = sample_fmt; //采样格式
    av_channel_layout_copy(&frame->ch_layout, channel_layout);//声道布局
    frame->sample_rate = sample_rate;//采样率
    frame->nb_samples = nb_samples;//采样大小

    if (nb_samples) {
        if (av_frame_get_buffer(frame, 0) < 0) {
            LOGE("Error allocating an audio buffer\n");
            exit(1);
        }
    }

    return frame;
}
static void open_audio(AVFormatContext *oc, const AVCodec *codec,
                       OutputStream *ost, AVDictionary *opt_arg)
{
    AVCodecContext *c;
    int nb_samples;
    int ret;
    AVDictionary *opt = NULL;

    c = ost->enc;

    /* open it */
    //拷贝字典参数
    av_dict_copy(&opt, opt_arg, 0);

    //打开编码器，释放参数字典
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        LOGE("Could not open audio codec: %s\n", av_err2str(ret));
        exit(1);
    }

    /* init signal generator */

    //初始化信号生成器，用于声音自动生成

    ost->t     = 0;
    ost->tincr = 2 * M_PI * 110.0 / c->sample_rate;
    /* increment frequency by 110 Hz per second */
    ost->tincr2 = 2 * M_PI * 110.0 / c->sample_rate / c->sample_rate;

    if (c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
        nb_samples = 10000;
    else
        nb_samples = c->frame_size;


    //分配音频帧和临时音频帧
    ost->frame     = alloc_audio_frame(c->sample_fmt, &c->ch_layout,
                                       c->sample_rate, nb_samples);
    ost->tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_S16, &c->ch_layout,
                                       c->sample_rate, nb_samples);

    /* copy the stream parameters to the muxer */
    //从CodecContext中拷贝参数到流/复用器
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        LOGE("Could not copy the stream parameters\n");
        exit(1);
    }

    /* create resampler context */
    //创建重采样配置，设定声道数、输入输出采样率、采样格式等选项
    ost->swr_ctx = swr_alloc();
    if (!ost->swr_ctx) {
        LOGE("Could not allocate resampler context\n");
        exit(1);
    }

    /* set options */
    av_opt_set_chlayout  (ost->swr_ctx, "in_chlayout",       &c->ch_layout,      0);
    av_opt_set_int       (ost->swr_ctx, "in_sample_rate",     c->sample_rate,    0);
    av_opt_set_sample_fmt(ost->swr_ctx, "in_sample_fmt",      AV_SAMPLE_FMT_S16, 0);
    av_opt_set_chlayout  (ost->swr_ctx, "out_chlayout",      &c->ch_layout,      0);
    av_opt_set_int       (ost->swr_ctx, "out_sample_rate",    c->sample_rate,    0);
    av_opt_set_sample_fmt(ost->swr_ctx, "out_sample_fmt",     c->sample_fmt,     0);

    /* initialize the resampling context */
    if ((ret = swr_init(ost->swr_ctx)) < 0) {
        LOGE("Failed to initialize the resampling context\n");
        exit(1);
    }

    LOGD("open_audio codec->name=%s",codec->name);
}
/* Prepare a dummy image. */
static void fill_yuv_image(AVFrame *pict, int frame_index,
                           int width, int height)
{
    int x, y, i;

    i = frame_index;

    /* Y */
    //生成Y
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;

    /* Cb and Cr */
    //生成Cb和Cr  注意下面的循环，可以明白为什么视频的宽和高必须是2的倍数了吧
    for (y = 0; y < height / 2; y++) {
        for (x = 0; x < width / 2; x++) {
            pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
            pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
        }
    }
}
static AVFrame *get_video_frame(OutputStream *ost)
{
    AVCodecContext *c = ost->enc;

    /* check if we want to generate more frames */
    //检查是否继续生成视频帧。如果超过预定时长就停止生成
    if (av_compare_ts(ost->next_pts, c->time_base,
                      STREAM_DURATION, (AVRational){ 1, 1 }) > 0)
        return NULL;

    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally; make sure we do not overwrite it here */

    //使帧数据可写，此处视频数据是代码生成的，注意frame指针本身不可以修改
    //因为FFmpeg内部会引用这个指针，一旦改了可能会破坏视频
    if (av_frame_make_writable(ost->frame) < 0)
        exit(1);

    //如果目标格式不是YUV420P，那么必须要进行格式转换
    if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
        /* as we only generate a YUV420P picture, we must convert it
         * to the codec pixel format if needed */
        if (!ost->sws_ctx) {//先获取转换环境
            ost->sws_ctx = sws_getContext(c->width, c->height,
                                          AV_PIX_FMT_YUV420P,
                                          c->width, c->height,
                                          c->pix_fmt,
                                          SCALE_FLAGS, NULL, NULL, NULL);
            if (!ost->sws_ctx) {
                LOGE("Could not initialize the conversion context\n");
                exit(1);
            }
        }
        //向临时帧填充数据，之后转换填入当前帧
        fill_yuv_image(ost->tmp_frame, ost->next_pts, c->width, c->height);
        sws_scale(ost->sws_ctx, (const uint8_t * const *) ost->tmp_frame->data,
                  ost->tmp_frame->linesize, 0, c->height, ost->frame->data,
                  ost->frame->linesize);
    } else {
        //目标格式就是YUV420P，直接转换就可以
        fill_yuv_image(ost->frame, ost->next_pts, c->width, c->height);
    }
//新帧生成，PTS递增
    ost->frame->pts = ost->next_pts++;

    return ost->frame;
}


static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)
{
//    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;
//
//    LOGD("log_packet pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
//           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
//           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
//           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
//           pkt->stream_index);
}
static int write_frame(AVFormatContext *fmt_ctx, AVCodecContext *c,
                       AVStream *st, AVFrame *frame, AVPacket *pkt)
{
    int ret;

    // send the frame to the encoder
    // 将帧数据送入编码配置上下文
    ret = avcodec_send_frame(c, frame);
    if (ret < 0) {
        LOGE("Error sending a frame to the encoder: %s\n",
                av_err2str(ret));
        exit(1);
    }

    while (ret >= 0) {//循环接收数据包，直到所有包接收完成
        ret = avcodec_receive_packet(c, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0) {
            LOGE("Error encoding a frame: %s\n", av_err2str(ret));
            exit(1);
        }

        /* rescale output packet timestamp values from codec to stream timebase */
        //转换时间戳，由数据包向视频流
        av_packet_rescale_ts(pkt, c->time_base, st->time_base);
        pkt->stream_index = st->index;//指明包属于哪个流

        /* Write the compressed frame to the media file. */
        log_packet(fmt_ctx, pkt);
        ret = av_interleaved_write_frame(fmt_ctx, pkt);//将包写入流
        /* pkt is now blank (av_interleaved_write_frame() takes ownership of
         * its contents and resets pkt), so that no unreferencing is necessary.
         * This would be different if one used av_write_frame(). */
        if (ret < 0) {
            LOGE("Error while writing output packet: %s\n", av_err2str(ret));
            exit(1);
        }
    }

    return ret == AVERROR_EOF ? 1 : 0;
}

/*
 * encode one video frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
static int write_video_frame(AVFormatContext *oc, OutputStream *ost)
{
    return write_frame(oc, ost->enc, ost->st, get_video_frame(ost), ost->tmp_pkt);
}

/* Prepare a 16 bit dummy audio frame of 'frame_size' samples and
 * 'nb_channels' channels. */
static AVFrame *get_audio_frame(OutputStream *ost)
{
    AVFrame *frame = ost->tmp_frame;
    int j, i, v;
    int16_t *q = (int16_t*)frame->data[0];

    /* check if we want to generate more frames */

    //检查是否继续生成音频帧。如果超过预定时长就停止生成
    if (av_compare_ts(ost->next_pts, ost->enc->time_base,
                      STREAM_DURATION, (AVRational){ 1, 1 }) > 0)
        return NULL;

    for (j = 0; j <frame->nb_samples; j++) {
        v = (int)(sin(ost->t) * 10000);
        for (i = 0; i < ost->enc->ch_layout.nb_channels; i++)
            *q++ = v;
        ost->t     += ost->tincr;
        ost->tincr += ost->tincr2;
    }

    frame->pts = ost->next_pts;
    ost->next_pts  += frame->nb_samples;

    return frame;
}
/*
 * encode one audio frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
static int write_audio_frame(AVFormatContext *oc, OutputStream *ost)
{
    AVCodecContext *c;
    AVFrame *frame;
    int ret;
    int dst_nb_samples;

    c = ost->enc;

    frame = get_audio_frame(ost);

    if (frame) {
        /* convert samples from native format to destination codec format, using the resampler */
        /* compute destination number of samples */
        dst_nb_samples = av_rescale_rnd(swr_get_delay(ost->swr_ctx, c->sample_rate) + frame->nb_samples,
                                        c->sample_rate, c->sample_rate, AV_ROUND_UP);
        av_assert0(dst_nb_samples == frame->nb_samples);

        /* when we pass a frame to the encoder, it may keep a reference to it
         * internally;
         * make sure we do not overwrite it here
         */
        ret = av_frame_make_writable(ost->frame);
        if (ret < 0)
            exit(1);

        /* convert to destination format */
        ret = swr_convert(ost->swr_ctx,
                          ost->frame->data, dst_nb_samples,
                          (const uint8_t **)frame->data, frame->nb_samples);
        if (ret < 0) {
            LOGE("Error while converting\n");
            exit(1);
        }
        frame = ost->frame;

        frame->pts = av_rescale_q(ost->samples_count, (AVRational){1, c->sample_rate}, c->time_base);
        ost->samples_count += dst_nb_samples;
    }

    return write_frame(oc, c, ost->st, frame, ost->tmp_pkt);
}
static void close_stream(AVFormatContext *oc, OutputStream *ost)
{
    avcodec_free_context(&ost->enc);
    av_frame_free(&ost->frame);
    av_frame_free(&ost->tmp_frame);
    av_packet_free(&ost->tmp_pkt);
    sws_freeContext(ost->sws_ctx);
    swr_free(&ost->swr_ctx);
}
/* Add an output stream. */
static void add_stream(OutputStream *ost, AVFormatContext *oc,
                       const AVCodec **codec,
                       enum AVCodecID codec_id)
{
    AVCodecContext *c;
    int i;
    LOGD("add_stream codec_id=%d",codec_id);
    /* find the encoder */
    // 根据推断出的格式，寻找相应的AVCodec编码
    *codec = avcodec_find_encoder(codec_id);
    if (!(*codec)) {
       LOGE("Could not find encoder for '%s'\n",avcodec_get_name(codec_id));
        return;
    }
    // 分配临时包
    ost->tmp_pkt = av_packet_alloc();
    if (!ost->tmp_pkt) {
        LOGE("Could not allocate AVPacket\n");
        return;
    }
    //分配一个视频/音频流，这里的ost就是本文一开头分析的OutputStream结构数据
    ost->st = avformat_new_stream(oc, NULL);
    if (!ost->st) {
        LOGE("Could not allocate stream\n");
        return;
    }
    //设定流ID号，与流在文件中的序号对应（一个文件中可以有多个视频/音频流）
    ost->st->id = oc->nb_streams-1;

    //分配CodecContext编码上下文，存入OutputStream结构
    c = avcodec_alloc_context3(*codec);
    if (!c) {
        LOGE("Could not alloc an encoding context\n");
        return;
    }
    ost->enc = c;


    //根据视频、音频不同类型，初始化CodecContext编码配置
    switch ((*codec)->type) {
        case AVMEDIA_TYPE_AUDIO:{ //这部分是音频数据
            c->sample_fmt  = (*codec)->sample_fmts ?
                             (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;//采样格式
            c->bit_rate    = 64000;//码率
            c->sample_rate = 44100;//采样速率
            if ((*codec)->supported_samplerates) {
                c->sample_rate = (*codec)->supported_samplerates[0];
                for (i = 0; (*codec)->supported_samplerates[i]; i++) {
                    if ((*codec)->supported_samplerates[i] == 44100)
                        c->sample_rate = 44100;
                }
            }

            AVChannelLayout src = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
            av_channel_layout_copy(&c->ch_layout, &src);//声道布局

            //声音的采样是指一秒内采集多少次声音数据，采样频率越高声音质量越好，44.1kHz就可以达到CD音响质量，也是MPEG标准声音质量。那么它的基准就是1/44100。
            ost->st->time_base = (AVRational){ 1, c->sample_rate };//计时基准
            break;
        }
        case AVMEDIA_TYPE_VIDEO:{//这部分是视频数据
            c->codec_id = codec_id;//视频编码

            c->bit_rate = 400000; //码率
            /* Resolution must be a multiple of two. */
            c->width    = 270;//视频宽高，注意必须是双数，YUV420P格式要求
            c->height   = 480;
            /* timebase: This is the fundamental unit of time (in seconds) in terms
             * of which frame timestamps are represented. For fixed-fps content,
             * timebase should be 1/framerate and timestamp increments should be
             * identical to 1. */

            //对于视频，我们知道人眼视觉残留的时间是1/24秒，视频只要达到每秒24帧以上人就不会觉得有闪烁或卡顿，一般会设成25，
            // 也就是代码中的STREAM_FRAME_RATE常数，视频time_base设为1/25，也就是每一个视频帧停留1/25秒
            ost->st->time_base = (AVRational){ 1, STREAM_FRAME_RATE };//计时基准


            c->time_base       = ost->st->time_base;

            c->gop_size      = 12; /* emit one intra frame every twelve frames at most */
            c->pix_fmt       = STREAM_PIX_FMT;
            if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
                /* just for testing, we also add B-frames */
                c->max_b_frames = 2;
            }
            if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
                /* Needed to avoid using macroblocks in which some coeffs overflow.
                 * This does not happen with normal video, it just happens here as
                 * the motion of the chroma plane does not match the luma plane. */
                c->mb_decision = 2;
            }
            break;
        }
        default:
            break;
    }

    /* Some formats want stream headers to be separate. */
    //是否需要分离的Stream Header
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}


extern "C" JNIEXPORT jint JNICALL
Java_com_smartdevice_ffmpeg_MainActivity_mux(JNIEnv *env, jobject thiz, jstring file_path) {
    const char *filename = env->GetStringUTFChars(file_path, 0);
    LOGD("filename=%s",filename);
    OutputStream video_st = { 0 }, audio_st = { 0 };
    const AVCodec *audio_codec, *video_codec;
    int have_video = 0, have_audio = 0;
    int encode_video = 0, encode_audio = 0;
    const AVOutputFormat *fmt;
    AVFormatContext  *oc;
    AVDictionary *opt = NULL;
    int ret;
    /* allocate the output media context */
    //  初始化了AVFormatContext（格式配置）

    //avformat_alloc_output_context2
    // 第二个参数可以是一个AVFormat实例，用来决定视频/音频格式，
    // 如果被设为NULL就继续看第三个参数，这是一个描述格式的字符串，比如可以是“h264"、 "mpeg"等；如果它也是NULL，
    // 就看最后第四个filename，从它的扩展名来推断应该使用的格式。
    //
    avformat_alloc_output_context2(&oc,NULL,NULL,filename);
    if (!oc){
        LOGE("Could not deduce output format from file extension: using MPEG FAIL");
        avformat_alloc_output_context2(&oc,NULL,"mpeg",filename);
    }
    if (!oc){
        LOGE("avformat_alloc_output_context2 mpeg  FAIL");
        return -1;
    }

    fmt = oc->oformat;
    LOGD("fmt->name=%s",fmt->name);
    /* Add the audio and video streams using the default format codecs
     * and initialize the codecs. */
    if (fmt->video_codec != AV_CODEC_ID_NONE){
        have_video=1;
        encode_video=1;
        // 添加视频流
        add_stream(&video_st,oc,&video_codec,fmt->video_codec);
    }

    if (fmt->audio_codec != AV_CODEC_ID_NONE){
        have_audio=1;
        encode_audio=1;
        // 添加音频流
        add_stream(&audio_st, oc, &audio_codec, fmt->audio_codec);
    }

    /* Now that all the parameters are set, we can open the audio and
        * video codecs and allocate the necessary encode buffers. */
    if(have_video){
        open_video(oc,video_codec,&video_st,opt);
    }
    if (have_audio){
        open_audio(oc, audio_codec, &audio_st, opt);
    }
    av_dump_format(oc, 0, filename, 1);

    /* open the output file, if needed */
    if (!(fmt->flags & AVFMT_NOFILE)) {
        //打开输出文件
        ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            LOGE("Could not open '%s': %s\n", filename,
                    av_err2str(ret));
            return 1;
        }
    }
    LOGD("avformat_write_header");
    /* Write the stream header, if any. */
    //输出流的头部
    ret = avformat_write_header(oc, &opt);
    if (ret < 0) {
        LOGE( "Error occurred when opening output file: %s\n",
                av_err2str(ret));
        return 1;
    }

    // 写入音视频数据
    while (encode_video || encode_audio) {
        /* select the stream to encode */
        if (encode_video &&
            (!encode_audio || av_compare_ts(video_st.next_pts, video_st.enc->time_base,
                                            audio_st.next_pts, audio_st.enc->time_base) <= 0)) {
            encode_video = !write_video_frame(oc, &video_st);
        } else {
            encode_audio = !write_audio_frame(oc, &audio_st);
        }
    }
    LOGD("av_write_trailer");
    //写尾部
    av_write_trailer(oc);

    /* Close each codec. */
    if (have_video)
        close_stream(oc, &video_st);//关闭视频流
    if (have_audio)
        close_stream(oc, &audio_st);//关闭音频流

    if (!(fmt->flags & AVFMT_NOFILE))
        /* Close the output file. */
        avio_closep(&oc->pb);//关闭输出文件

    /* free the stream */
    avformat_free_context(oc);//释放格式配置上下文
    env->ReleaseStringUTFChars(file_path, filename);

    return 0;
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_smartdevice_ffmpeg_MainActivity_demux(JNIEnv *env, jobject thiz, jstring file_path) {
    const char *filename = env->GetStringUTFChars(file_path, 0);
    LOGD("demux filename=%s",filename);
    example_demux(filename);
    env->ReleaseStringUTFChars(file_path, filename);
    return 0;
}