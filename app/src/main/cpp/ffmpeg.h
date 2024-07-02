//
// Created by hq on 2018/12/26.
//

#ifndef SKYAPP_FFMPEG_H
#define SKYAPP_FFMPEG_H

#include <android/log.h>
#include <jni.h>

#define TAG "BRUCE"
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG , TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO , TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN , TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR , TAG, __VA_ARGS__)

int getVersion();
int example_demux (const char *filePath);
int example_remux (const char *filePath);
int example_avio_reading (const char *filePath);
int example_decode(const char *filePath);
#endif //SKYAPP_FFMPEG_H
