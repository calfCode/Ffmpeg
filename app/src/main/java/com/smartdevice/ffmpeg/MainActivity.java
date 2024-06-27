package com.smartdevice.ffmpeg;

import androidx.appcompat.app.AppCompatActivity;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.MediaStore;
import android.util.Log;
import android.widget.TextView;

import com.smartdevice.ffmpeg.databinding.ActivityMainBinding;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.OutputStreamWriter;

public class MainActivity extends AppCompatActivity {

    private static final String TAG = "MainActivity_TEST";

    // Used to load the 'ffmpeg' library on application startup.
    static {
        System.loadLibrary("ffmpeg");
    }

    private ActivityMainBinding binding;

    private boolean isMuxdebug = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        // Example of a call to a native method
        TextView tv = binding.sampleText;
        tv.setText("Ffmpeg");
        String pk = getPackageName();
        String fileName;
        if (isMuxdebug) {

            File parentFile = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DCIM);
            if (parentFile != null) {
                Log.d(TAG, "parentFile=" + parentFile.getAbsolutePath());
            }


            fileName = "test_" + System.currentTimeMillis() + ".mp4";
            Log.d(TAG, "fileName=" + fileName);
            String filePath = "/data/user/0/" + pk + "/app_video/" + fileName;
            filePath = parentFile.getAbsolutePath() + File.separator + fileName;

            handleFile(filePath);
        }
        fileName="test.mp4";
        String filePath = "/data/user/0/" + pk + "/app_video/" + fileName;

        AssectsUtil.loadAssetsDirfile(getApplicationContext(),"video");

        Log.d(TAG, "filePath=" + filePath);
        demux(filePath);

    }

    /**
     * A native method that is implemented by the 'ffmpeg' native library,
     * which is packaged with this application.
     */
    public native  int handleFile(String filePath);

    public native  int demux(String filePath);

}