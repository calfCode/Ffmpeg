package com.smartdevice.ffmpeg;

import androidx.appcompat.app.AppCompatActivity;

import android.os.Bundle;
import android.os.Environment;
import android.util.Log;
import android.widget.TextView;

import com.smartdevice.ffmpeg.databinding.ActivityMainBinding;

import java.io.File;

public class MainActivity extends AppCompatActivity {

    private static final String TAG = "MainActivity_TEST";

    // Used to load the 'ffmpeg' library on application startup.
    static {
        System.loadLibrary("ffmpeg");
    }

    private ActivityMainBinding binding;

    private TestType testType = TestType.RESAMPLE_AUDIO;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        // Example of a call to a native method
        TextView tv = binding.sampleText;
        tv.setText("debug");
        String pk = getPackageName();
        String fileName;
        String filePath=null;
        String assertDir = "video";

        File parentFile = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        String parentPath = parentFile.getAbsolutePath()+File.separator;
        String appFilePath = AssectsUtil.getAppDir(getApplicationContext(),assertDir)+File.separator;

        Log.d(TAG, "appFilePath=" + appFilePath);
        if (testType == TestType.MUX) {
            if (parentFile != null) {
                Log.d(TAG, "parentFile=" + parentFile.getAbsolutePath());
            }

            fileName = "test_" + System.currentTimeMillis() + ".mp4";
            Log.d(TAG, "fileName=" + fileName);
            filePath = parentPath + fileName;

            mux(filePath);
        } else if (testType == TestType.DEMUX) {
            AssectsUtil.loadAssetsDirfile(getApplicationContext(),assertDir);
            fileName = "test.mp4";
            filePath = appFilePath + fileName;
            String audioFileName = parentPath +"demux_audio.a";
            String videoFileName = parentPath +"demux_video.v";
            Log.d(TAG, "fileName=" + fileName);
            demux(filePath,audioFileName,videoFileName);
        }else if(testType == TestType.REMUX){
            AssectsUtil.loadAssetsDirfile(getApplicationContext(),assertDir);
            fileName = "test.mp4";
            filePath = appFilePath + fileName;
            exampleRemux(filePath);
        } else if(testType == TestType.AVIO_READ){
            AssectsUtil.loadAssetsDirfile(getApplicationContext(),assertDir);
            fileName = "test.mp4";
            filePath = appFilePath + fileName;
            exampleAvioReading(filePath);
        }else if(testType == TestType.DECODE){
            AssectsUtil.loadAssetsDirfile(getApplicationContext(),assertDir);
            fileName = "test.mp4";
            filePath = appFilePath + fileName;
            String audioFileName = parentPath +"decode.a";
            String videoFileName = parentPath +"decode.v";
            exampleDecode(filePath,audioFileName,videoFileName);
        }else if(testType == TestType.AUDIO_ENCODE){
            AssectsUtil.loadAssetsDirfile(getApplicationContext(),assertDir);
            fileName = "test.mp2";
            filePath = appFilePath + fileName;
            exampleAudioEncode(filePath);
        }else if(testType == TestType.VIDEO_ENCODE){
            AssectsUtil.loadAssetsDirfile(getApplicationContext(),assertDir);
            fileName = "test.v";
            filePath = appFilePath + fileName;
            exampleVideoEncode(filePath);
        }else if(testType == TestType.VIDEO_DECODE){
            AssectsUtil.loadAssetsDirfile(getApplicationContext(),assertDir);
            fileName = "test.v";
            filePath = appFilePath + fileName;
            String videoFileName = parentPath +"decode.v";
            exampleVideoDecode(filePath,videoFileName);
        }else if(testType == TestType.AUDIO_DECODE){
            AssectsUtil.loadAssetsDirfile(getApplicationContext(),assertDir);
            fileName = "test.mp2";
            filePath = appFilePath + fileName;
            String audioFileName = parentPath +"decode.a";
            exampleAudioDecode(filePath,audioFileName);
        }else if(testType == TestType.VIDEO_DECODE_FILTER){
            AssectsUtil.loadAssetsDirfile(getApplicationContext(),assertDir);
            fileName = "test.v";
            filePath = appFilePath + fileName;
            final String videoPath = filePath;
            final String videoFileName = parentPath +"decode_filter.v";
            new Thread(new Runnable() {
                @Override
                public void run() {
                    exampleVideoDecodeFilter(videoPath,videoFileName);
                }
            }).start();

        }else if(testType == TestType.AUDIO_DECODE_FILTER){
            AssectsUtil.loadAssetsDirfile(getApplicationContext(),assertDir);
            fileName = "test.mp2";
            filePath = appFilePath + fileName;
            String audioFileName = parentPath +"decode_filter.a";
            exampleAudioDecodeFilter(filePath,audioFileName);
        }else if(testType == TestType.FILTER_AUDIO){
            fileName = "filter_audio.a";
            Log.d(TAG, "fileName=" + fileName);
            filePath = parentPath + fileName;
            String outputPath =filePath;
            Log.d(TAG, "outputPath=" + outputPath);
            new Thread(new Runnable() {
                @Override
                public void run() {
                    exampleFilterAudio(5,outputPath);
                }
            }).start();
        }else if(testType == TestType.RESAMPLE_AUDIO){
            fileName = "resample.a";
            Log.d(TAG, "fileName=" + fileName);
            filePath = parentPath + fileName;
            String outputPath =filePath;
            Log.d(TAG, "outputPath=" + outputPath);
            new Thread(new Runnable() {
                @Override
                public void run() {
                    exampleResampleAudio(outputPath);
                }
            }).start();
        }
        Log.d(TAG, "filePath=" + filePath);

    }

    /**
     * A native method that is implemented by the 'ffmpeg' native library,
     * which is packaged with this application.
     */
    public native int mux(String filePath);
    public native int demux(String filePath,String audioOutPath,String videoOutPath);

//    public native int demuxExample(String filePath);
    public native int exampleRemux(String filePath);
    public native int exampleAvioReading(String filePath);
    public native int exampleDecode(String filePath,String audioOutPath,String videoOutPath);
    public native int exampleAudioEncode(String filePath);
    public native int exampleVideoEncode(String filePath);

    public native int exampleAudioDecode(String filePath,String outputPath);
    public native int exampleVideoDecode(String filePath,String outputPath);

    public native int exampleAudioDecodeFilter(String filePath,String outputPath);
    public native int exampleVideoDecodeFilter(String filePath,String outputPath);

    public native int exampleFilterAudio(int duration,String outputPath);
    public native int exampleResampleAudio(String outputPath);
}