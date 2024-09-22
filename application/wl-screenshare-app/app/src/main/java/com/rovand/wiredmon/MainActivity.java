package com.rovand.wiredmon;

import static android.icu.lang.UCharacter.GraphemeClusterBreak.T;

import android.content.Context;
import android.content.Intent;
import android.hardware.usb.UsbManager;
import android.media.MediaFormat;
import android.os.Bundle;

import androidx.appcompat.app.AppCompatActivity;

import android.util.Log;
import android.view.View;

import com.rovand.wiredmon.webusb.ShareScreenActivity;

import android.widget.Button;
import android.widget.EditText;
import android.widget.Toast;

public class MainActivity extends AppCompatActivity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        setContentView(R.layout.activity_main);
        Button h264 = findViewById(R.id.btn_h264);
        h264.setOnClickListener(new SelectCodecClick(MediaFormat.MIMETYPE_VIDEO_AVC));
        Button h265 = findViewById(R.id.btn_h265);
        h265.setOnClickListener(new SelectCodecClick(MediaFormat.MIMETYPE_VIDEO_HEVC));
    }

    public class SelectCodecClick implements View.OnClickListener {

        String mode = MediaFormat.MIMETYPE_VIDEO_AVC;

        public SelectCodecClick(String _mode) {
            mode = _mode;

            if (mode != MediaFormat.MIMETYPE_VIDEO_AVC && mode != MediaFormat.MIMETYPE_VIDEO_HEVC) {
                mode = MediaFormat.MIMETYPE_VIDEO_AVC;
                Toast.makeText(MainActivity.this, "Something went wrong", Toast.LENGTH_SHORT).show();
            }
        }

        @Override
        public void onClick(View v) {
            Intent intent = new Intent(getApplicationContext(), ShareScreenActivity.class);
            if (intent != null) {
                UsbManager usbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
                if (usbManager.getAccessoryList() != null && usbManager.getAccessoryList().length > 0)
                    intent.putExtra(UsbManager.EXTRA_ACCESSORY, usbManager.getAccessoryList()[0]);

                try {
                    EditText txtIp = (EditText) findViewById(R.id.txt_ip);
                    intent.putExtra("VMODE", mode);
                    intent.putExtra("SMODE", "socket");
                    intent.putExtra("IP", txtIp.getText().toString());
//                intent.putExtra("SMODE", "usb");
                    startActivity(intent);
                }catch(Exception e) {
                    Log.d("GIAMMI", e.toString());
                }
            }
        }
    }
}
