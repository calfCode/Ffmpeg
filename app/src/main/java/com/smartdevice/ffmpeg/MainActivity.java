package com.smartdevice.ffmpeg;

import androidx.appcompat.app.AppCompatActivity;

import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

import com.smartdevice.ffmpeg.databinding.ActivityMainBinding;

import java.io.File;

public class MainActivity extends AppCompatActivity {

    private static final String TAG = "MainActivity";

    // Used to load the 'ffmpeg' library on application startup.
    static {
        System.loadLibrary("ffmpeg");
    }

    private ActivityMainBinding binding;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        // Example of a call to a native method
        TextView tv = binding.sampleText;
        tv.setText(stringFromJNI());
        AssectsUtil.loadAssetsDirfile(getApplicationContext(),"video");
        String pk = getPackageName();
        File file = new File("/data/user/0/"+pk+"/app_video/video_960_20s.mp4");
        if (file.exists()) {
            Log.d(TAG, file.getAbsolutePath() + " exists");
            handleFile(file.getAbsolutePath());
        }else{
            Log.d(TAG, file.getAbsolutePath() + " not exists");
        }
    }

    /**
     * A native method that is implemented by the 'ffmpeg' native library,
     * which is packaged with this application.
     */
    public native String stringFromJNI();
    public native  int handleFile(String filePath);

}