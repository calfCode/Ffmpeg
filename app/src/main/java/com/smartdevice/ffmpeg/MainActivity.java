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

    private TestType testType = TestType.AVIO_READ;

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
        if (testType == TestType.MUX) {
            File parentFile = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DCIM);
            if (parentFile != null) {
                Log.d(TAG, "parentFile=" + parentFile.getAbsolutePath());
            }


            fileName = "test_" + System.currentTimeMillis() + ".mp4";
            Log.d(TAG, "fileName=" + fileName);
            filePath = parentFile.getAbsolutePath() + File.separator + fileName;

            mux(filePath);
        } else if (testType == TestType.DEMUX) {
            AssectsUtil.loadAssetsDirfile(getApplicationContext(),"video");
            fileName = "test.mp4";
            filePath = "/data/user/0/" + pk + "/app_video/" + fileName;
            demux(filePath);
        }else if(testType == TestType.REMUX){
            AssectsUtil.loadAssetsDirfile(getApplicationContext(),"video");
            fileName = "test.mp4";
            filePath = "/data/user/0/" + pk + "/app_video/" + fileName;
            exampleRemux(filePath);
        } else if(testType == TestType.AVIO_READ){
            AssectsUtil.loadAssetsDirfile(getApplicationContext(),"video");
            fileName = "test.mp4";
            filePath = "/data/user/0/" + pk + "/app_video/" + fileName;
            exampleAvioReading(filePath);
        }
        Log.d(TAG, "filePath=" + filePath);

    }

    /**
     * A native method that is implemented by the 'ffmpeg' native library,
     * which is packaged with this application.
     */
    public native int mux(String filePath);
    public native int demux(String filePath);

//    public native int demuxExample(String filePath);
    public native int exampleRemux(String filePath);
    public native int exampleAvioReading(String filePath);
}