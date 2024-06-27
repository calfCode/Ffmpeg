package com.smartdevice.ffmpeg;

import android.content.Context;
import android.content.res.AssetManager;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

public class AssectsUtil {
    private static final String TAG = "AssectsUtil";

    public static void loadAssetsDirfile(Context mContext, String srcDir)
    {
        if (mContext == null)
        {
            Log.d(TAG ,"LoadAssetsMenufile context = null");
            return;
        }
        String path = getTvDir(mContext,srcDir);
        Log.d(TAG,"loadAssetsDirfile::path == " + path);
        AssectsUtil.deleteFilesByDirectory(new File(path));
        try
        {
            String str[] = mContext.getAssets().list(srcDir);
            Log.d(TAG , "str.length is ---" + str.length);
            if (str.length > 0)
            {
                for (String stringTmp : str)
                {
                    Log.d(TAG , "stringTmp=" + stringTmp);
                    AssectsUtil.copyAssets(path, srcDir, stringTmp, mContext);
                }
            }
        } catch (IOException e)
        {
            // TODO Auto-generated catch block
            e.printStackTrace();
        }
    }

    public static String getAssetsMenufile(Context mContext, String srcDir, String menu_name)
    {
        String path = getTvDir(mContext,srcDir);
        path = path + File.separator + menu_name;
        Log.d(TAG,"path==="+path);
        File filePath = new File(path);
        if (filePath.exists())
        {
            return path;
        } else
        {
            return null;
        }
    }

    private static void copyAssets(String path, String srcDir, String filename, Context context)
    {
        try
        {

            File fpath = new File(path);
            if (!fpath.isDirectory())
            {
                fpath.mkdirs();
            }
            File pathfile = new File(path + File.separator + filename);
            Log.d(TAG,"pathfile="+pathfile.getAbsolutePath());
            if (pathfile.exists())
            {
                Log.d(TAG,"pathfile A");
                return;
            }
            AssetManager assetManager = context.getAssets();
            Log.d(TAG,"pathfile srcDir + File.separator + filename="+(srcDir + File.separator + filename));
            InputStream inputStream = assetManager.open(srcDir + File.separator + filename);
            FileOutputStream fileOutputStream = new FileOutputStream(path + File.separator
                    + filename);
            byte[] buffer = new byte[1024];
            int read;
            while ((read = inputStream.read(buffer)) != -1)
            {
//                Log.d(TAG,"copyAssets read="+read);
                fileOutputStream.write(buffer, 0, read);
            }
            inputStream.close();
            inputStream = null;
            fileOutputStream.flush();
            fileOutputStream.close();
            fileOutputStream = null;
            // SkySystemUtil.execCmd(LINUX_CMD.LINUX_CMD_CHMOD, "-R 777 " + path + File.separator
            // + srcDir);
        } catch (Exception e)
        {
            // TODO: handle exception
        }
    }

    private static void deleteFilesByDirectory(File directory)
    {
        if (directory != null && directory.exists())
        {
            Log.d(TAG ,"delete : " + directory.getName());
            if (directory.isDirectory())
            {
                for (File item : directory.listFiles())
                {
                    if (item.isDirectory())
                    {
                        deleteFilesByDirectory(item);
                    } else
                    {
                        item.delete();
                    }
                }
            } else
            {
                directory.delete();
            }
        }
    }
    private static String getTvDir(Context mContext, String dir)
    {
        File tvDir = mContext.getDir(dir, Context.MODE_PRIVATE);
        return tvDir.getAbsolutePath();
    }
}
