package com.smartdevice.ffmpeg;

public enum TestType {
    DEBUG,
    MUX,
    DEMUX,
    REMUX,
    AVIO_READ,
    DECODE,
    AUDIO_ENCODE,
    VIDEO_ENCODE,

    AUDIO_DECODE,
    VIDEO_DECODE,
    AUDIO_DECODE_FILTER,
    VIDEO_DECODE_FILTER,
    FILTER_AUDIO,
    RESAMPLE_AUDIO,
    SCALE_VIDEO,
}
