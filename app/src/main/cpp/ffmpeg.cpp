//
// Created by hq on 2018/12/26.
//

#include "ffmpeg.h"
#include <csignal>
#include <csetjmp>
#include <libavutil/channel_layout.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
#include "libswscale/swscale.h"

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

int getVersion(){
   int version =  avutil_version();
    return version;
}

