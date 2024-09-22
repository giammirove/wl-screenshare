package com.rovand.wiredmon;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbAccessory;
import android.hardware.usb.UsbManager;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import android.widget.Toast;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;

import com.rovand.wiredmon.webusb.ShareScreenActivity;

import java.io.FileDescriptor;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Socket;

public abstract class ConnectedActivity extends AppCompatActivity {
    public static final String TAG = "GIAMMI";

    private static final String ACTION_USB_PERMISSION = ShareScreenActivity.class.getCanonicalName() + ".usb_permission";

    public static final String DEFAULT_SERVER_IP = "127.0.0.1"; //server IP address
    public static final int SERVER_PORT = 53516;

    /**
     * -Keep links to parcel descriptor and file descriptor in memory to avoid
     * "BAD FILE DESCRIPTOR ISSUE DURING COMMUNICATION"
     */
    protected ParcelFileDescriptor fileDescriptor;
    protected FileDescriptor fd;

    protected Socket socket;

    /**
     * ----------------------------------
     */

    protected OutputStream outputStream;
    protected InputStream inputStream;

    protected  UsbManager usbManager;
    protected UsbAccessory accessory;

    protected String mode;

    protected String smode;

    protected String ip;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (getIntent() == null) finish();

        mode = getIntent().getStringExtra("VMODE");
        smode = getIntent().getStringExtra("SMODE");
        ip = getIntent().getStringExtra("IP");
        if (ip == null)
            ip = DEFAULT_SERVER_IP;

        Log.d("GIAMMI", "VMODE " + mode);
        Log.d("GIAMMI", "SMODE " + smode);
        Log.d("GIAMMI", "IP  " + ip);

        if (smode.equals("socket")) {
            createConnection();
        } else {
            UsbAccessory accessory = (UsbAccessory) getIntent().getParcelableExtra(UsbManager.EXTRA_ACCESSORY);
            if (accessory == null) {
                Toast.makeText(this, "Something wen wrong! Try to reconnect.", Toast.LENGTH_SHORT).show();
                Log.d("GIAMMI", "Accessory not found");
                finish();
            }


            prepareToConnect(getIntent(), accessory);
        }
    }

    private final BroadcastReceiver mAccessoryPermissionReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (intent != null && intent.getAction().equals(UsbManager.ACTION_USB_ACCESSORY_DETACHED)) {
                finish();
                return;
            }
            Log.d("GIAMMI", "[BROADCAST]");
            UsbAccessory accessory = (UsbAccessory) intent.getParcelableExtra(UsbManager.EXTRA_ACCESSORY);
            prepareToConnect(intent, accessory);
        }
    };

    private void prepareToConnect(Intent intent, UsbAccessory accessory) {
        if (accessory == null)
            return;
        Log.d("GIAMMI", "Prepare to connect");
        this.accessory = accessory;

        UsbManager usbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
        if (!intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
            Log.d("GIAMMI", "Permission NOT granted");
            PendingIntent permissionIntent = PendingIntent.getBroadcast(this, 0,
                    new Intent(ACTION_USB_PERMISSION), PendingIntent.FLAG_MUTABLE);
            IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);
            filter.addAction(UsbManager.ACTION_USB_ACCESSORY_DETACHED);
            registerReceiver(mAccessoryPermissionReceiver, filter);
            Log.d("GIAMMI", "Requesting permission");
            usbManager.requestPermission(accessory, permissionIntent);
        } else {
            createConnection();
        }
    }

    public void createConnection() {
        try {
            onConnected();
        } catch (IOException e) {
            throw new RuntimeException(e);
        }
    }

    @Override
    protected void onDestroy() {
        if (accessory != null)
            unregisterReceiver(mAccessoryPermissionReceiver);
        super.onDestroy();
    }

    protected abstract void onConnected() throws IOException;
}
