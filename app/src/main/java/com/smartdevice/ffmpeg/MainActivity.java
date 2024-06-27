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

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        // Example of a call to a native method
        TextView tv = binding.sampleText;
        tv.setText("Ffmpeg");
//        AssectsUtil.loadAssetsDirfile(getApplicationContext(),"video");

        File parentFile = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DCIM);
        if (parentFile!=null) {
            Log.d(TAG, "parentFile=" + parentFile.getAbsolutePath());
        }

        String pk = getPackageName();
        String fileName = "test_"+System.currentTimeMillis()+".mp4";
        Log.d(TAG,"fileName="+fileName);
        String filePath ="/data/user/0/"+pk+"/app_video/"+fileName;
        filePath = parentFile.getAbsolutePath()+File.separator+fileName;
        Log.d(TAG,"filePath="+filePath);
        handleFile(filePath);
//        ContentResolver contentResolver = getContentResolver();
//        Uri videoCol = MediaStore.Video.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY);
//        ContentValues contentValues = new ContentValues();
//        contentValues.put(MediaStore.Video.Media.DISPLAY_NAME,fileName);
//        Uri newUri = contentResolver.insert(videoCol,contentValues);
//        Log.d(TAG,"newUri="+newUri.toString());
//        File file = new File(filePath);
//        if (file.exists()) {
//            try {
//                OutputStream outputStream = contentResolver.openOutputStream(newUri);
//                if (outputStream!=null) {
//                    InputStream inputStream = new FileInputStream(file);
//                    byte[] buffer = new byte[1024];
//                    int read;
//                    while ((read = inputStream.read(buffer)) != -1)
//                    {
//                        outputStream.write(buffer, 0, read);
//                    }
//                    inputStream.close();
//                    inputStream = null;
//                    outputStream.flush();
//                    outputStream.close();
//                }
//            } catch (FileNotFoundException e) {
//                Log.d(TAG, "e=" + e.getMessage());
//            } catch (IOException e) {
//                Log.d(TAG, "e=" + e.getMessage());
//            }
//        }
    }

    /**
     * A native method that is implemented by the 'ffmpeg' native library,
     * which is packaged with this application.
     */
    public native  int handleFile(String filePath);

}