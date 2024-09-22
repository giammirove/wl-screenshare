package com.rovand.wiredmon.webusb;

import android.content.Context;
import android.hardware.usb.UsbManager;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;

import com.rovand.wiredmon.ConnectedActivity;

import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.net.InetAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;


public class ShareScreenActivity extends ConnectedActivity {


    private PlayerThread mPlayer = null;


    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN,
                WindowManager.LayoutParams.FLAG_FULLSCREEN);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN,
                WindowManager.LayoutParams.FLAG_FULLSCREEN);

        WindowInsetsControllerCompat windowInsetsController =
                WindowCompat.getInsetsController(getWindow(), getWindow().getDecorView());
        windowInsetsController.setSystemBarsBehavior(
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        );
        windowInsetsController.hide(WindowInsetsCompat.Type.systemBars());
    }

    public class MySurfaceHolder implements SurfaceHolder.Callback {

        String mode;

        public MySurfaceHolder(String _mode) {
            mode = _mode;
        }

        @Override
        public void surfaceCreated(@NonNull SurfaceHolder holder) {

        }

        @Override
        public void surfaceChanged(@NonNull SurfaceHolder holder, int format, int width, int height) {
            if (mPlayer == null) {
                mPlayer = new PlayerThread(holder.getSurface(), mode);
                mPlayer.start();
            }
        }

        @Override
        public void surfaceDestroyed(@NonNull SurfaceHolder holder) {
            Log.d("GIAMMI" , "destroyed");
            Toast.makeText(getApplicationContext(), "Surface destroyed", Toast.LENGTH_SHORT).show();
            mPlayer.interrupt();
            holder.removeCallback(this);
        }
    }

    @Override
    protected void onConnected() throws IOException {
        Log.d("GIAMMI", "CONNECTED");
        SurfaceView sv = new SurfaceView(this);
        sv.getHolder().addCallback(new MySurfaceHolder(mode));
        setContentView(sv);

        //TODO: possible new feature
//        sv.setOnTouchListener(new View.OnTouchListener() {
//            @Override
//            public boolean onTouch(View v, MotionEvent event) {
//                int x = (int) event.getX();
//                int y = (int) event.getY();
//
//                Log.d(TAG, "TOUCH " + x + " - " + y);
//
//                if (socket != null && outputStream != null) {
//                    Thread sendCoords = new Thread() {
//                        public void run() {
//
//                            Log.d(TAG, "Send coords");
//                            try {
//                                byte xb[] = { (byte)((x >> 24) & 0xff), (byte)((x >> 16) & 0xff), (byte)((x >>8) & 0xff), (byte)((x) & 0xff)};
//                                byte yb[] = { (byte)((y >> 24) & 0xff), (byte)((y >> 16) & 0xff), (byte)((y >>8) & 0xff), (byte)((y) & 0xff)};
//                                outputStream.write(xb);
//                                outputStream.write(yb);
//                                outputStream.flush();
//                            } catch (IOException e) {
//                                logExc(e);
//                            }
//                        }
//                    };
//                    sendCoords.start();
//                }
//
//
//                switch (event.getAction()) {
//                    case MotionEvent.ACTION_DOWN:
//                        Log.i(TAG, "touched down");
//                        break;
//                    case MotionEvent.ACTION_MOVE:
//                        Log.i(TAG, "moving: (" + x + ", " + y + ")");
//                        break;
//                    case MotionEvent.ACTION_UP:
//                        Log.i(TAG, "touched up");
//                        break;
//                }
//                return true;
//            }
//        });
    }

    void logExc(Exception e) {
        Log.d("GIAMMI", Log.getStackTraceString(e));
    }


    private class PlayerThread extends Thread {
        private MediaCodec decoder;
        private Surface surface;

        private String mode;

        public PlayerThread(Surface surface, String mode) {
            this.surface = surface; this.mode = mode;
        }

        @Override
        public void run() {

            if (smode.equals("socket")) {
                try {
                    socketVersion();
                } catch (Exception e) {
                    try {
                        if (socket != null)
                            socket.close();
                    } catch (IOException ex) {
                        logExc(ex);
                    }
                    logExc(e);
                }
                Log.d("GIAMMI", "Terminated");
            } else {
                try {
                    usbVersion();
                } catch (Exception e) {
                    logExc(e);
                }

                usbCloseConnection();
            }

            try {
                if (decoder != null) {
                    decoder.stop();
                    decoder.release();
                }
            } catch (Exception ex) {
                logExc(ex);
            }

            if (socket != null) {
                try {
                    socket.close();
                    Log.d("GIAMMI", "Socket closed");
                } catch (IOException e) {
                    Log.d("GIAMMI", "Cannot close socket");
                }
            }

            if (Thread.interrupted())
                Log.d("GIAMMI", "thread interrupted");
            else
                Log.d("GIAMMI", "thread NOT interrupted");
            Log.d("GIAMMI", "stopped");
        }

        boolean usbInitConnection() {
            UsbManager usbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
            if (usbManager.getAccessoryList().length == 0) {
                Log.d("GIAMMI", "No device to init connection");
                return false;
            }
            do {
                fileDescriptor = usbManager.openAccessory(usbManager.getAccessoryList()[0]);
            } while (fileDescriptor == null);

            fd = fileDescriptor.getFileDescriptor();

            inputStream = new FileInputStream(fd);
            outputStream = new FileOutputStream(fd);
            return true;
        }

        void usbCloseConnection() {
            Log.d("GIAMMI", "Closing connection!");
            try {
                if (inputStream != null)
                    inputStream.close();
                if (outputStream != null)
                    outputStream.close();
                fd = null;
            } catch (IOException ex) {
                logExc(ex);
            }
        }

        public boolean usbVersion() throws Exception {

            Log.d("GIAMMI", "Running");

            int frameCounter = 0;

            final BlockingQueue<Integer> freeInputs = new ArrayBlockingQueue<>(20);

            int BUFFSIZE = 16384;
            int MAXSIZE = 1920 * 1080;
            byte[] buff = new byte[BUFFSIZE];
            byte[] total = new byte[MAXSIZE];
            int currRead = 0;
            int totalRead = 0;

            final int[] bufferedFPS = {0};
            final long[] bufferedBegin = {System.currentTimeMillis()};
            int frameRate = 30;

            if (!usbInitConnection())
                return false;

            MediaFormat format = MediaFormat.createVideoFormat(mode, 1920, 1080);


            decoder = MediaCodec.createDecoderByType(mode);

            decoder.setCallback(new MediaCodec.Callback() {
                @Override
                public void onInputBufferAvailable(@NonNull MediaCodec mediaCodec, int i) {
                    try {
                        freeInputs.put(i);
                    } catch (InterruptedException e) {

                    }
                }

                @Override
                public void onOutputBufferAvailable(@NonNull MediaCodec mediaCodec, int indexOut, @NonNull MediaCodec.BufferInfo info) {
                    try {
                        switch (indexOut) {
                            case MediaCodec.INFO_OUTPUT_FORMAT_CHANGED:
                                Log.d("GIAMMI", "New format " + decoder.getOutputFormat());
                                break;
                            case MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED:
                                break;
                            case MediaCodec.INFO_TRY_AGAIN_LATER:
                                Log.d("GIAMMI", "Try later!");
                                break;
                            default:
                                decoder.releaseOutputBuffer(indexOut, true);

                                bufferedFPS[0]++;
                                if (System.currentTimeMillis() - bufferedBegin[0] > 1000) {
                                    bufferedBegin[0] = System.currentTimeMillis();
//                                    Log.d("GIAMMI", "Buffered FPS " + bufferedFPS[0]);
                                    bufferedFPS[0] = 0;
                                }

                                break;
                        }
                    } catch (Exception e) {

                    }
                }

                @Override
                public void onError(@NonNull MediaCodec mediaCodec, @NonNull MediaCodec.CodecException e) {

                }

                @Override
                public void onOutputFormatChanged(@NonNull MediaCodec mediaCodec, @NonNull MediaFormat _mediaFormat) {
                    Log.d("GIAMMI", "New format " + decoder.getOutputFormat());
                }
            });

            decoder.configure(format, surface, null, 0);

            decoder.start();

            long begin = System.currentTimeMillis();
            int fps = 0;

            while ((currRead = inputStream.read(buff)) > 0) {

                System.arraycopy(buff, 0, total, totalRead, currRead);
                totalRead += currRead;

                if (currRead < BUFFSIZE) {

                    fps++;

                    if (System.currentTimeMillis() - begin > 1000) {
                        begin = System.currentTimeMillis();
//                            Log.d("GIAMMI", "FPS " + fps);
                        frameRate = fps;
                        fps = 0;
                    }

                    int inputIndex = -1;
                    try {
                        inputIndex = freeInputs.take();
                    } catch (InterruptedException e) {
                        throw new RuntimeException(e);
                    }

                    if (inputIndex != -1) {
                        ByteBuffer inputBuf = decoder.getInputBuffer(inputIndex);
                        inputBuf.clear();

                        inputBuf.put(total);

                        if (frameRate == 0) frameRate = 30;
                        try {
                            decoder.queueInputBuffer(inputIndex, 0, totalRead, 8 * (frameCounter++) / frameRate, 0);
                        }catch (Exception e) {
                            logExc(e);
                        }
                    }

                    Arrays.fill(total, (byte) 0);
                    totalRead = 0;
                }
            }

            return true;
        }

        public void socketVersion() {

            final BlockingQueue<Integer> freeInputs = new ArrayBlockingQueue<>(20);

            try {

                InetAddress serverAddr = InetAddress.getByName(ip);

                Log.d("GIAMMI-SOCK", "Connecting to " + ip + " ...");

//                Toast.makeText(getApplicationContext(), "Connecting to " + ip, Toast.LENGTH_SHORT).show();

                //create a socket to make the connection with the server
                socket = new Socket(serverAddr, SERVER_PORT);
                socket.setTcpNoDelay(true);

                Log.d("GIAMMI-SOCK", "Connected");
                
                inputStream =  socket.getInputStream();
                outputStream = socket.getOutputStream();

                int frameRate = 60;

                MediaFormat format = MediaFormat.createVideoFormat(mode, 1920, 1080);

                decoder = MediaCodec.createDecoderByType(mode);

                decoder.setCallback(new MediaCodec.Callback() {
                    @Override
                    public void onInputBufferAvailable(@NonNull MediaCodec mediaCodec, int i) {
                        try {
                            freeInputs.put(i);
                        } catch (InterruptedException e) {

                        }
                    }

                    @Override
                    public void onOutputBufferAvailable(@NonNull MediaCodec mediaCodec, int indexOut, @NonNull MediaCodec.BufferInfo info) {
                        try {
                            switch (indexOut) {
                                case MediaCodec.INFO_OUTPUT_FORMAT_CHANGED:
                                    Log.d("GIAMMI", "New format " + decoder.getOutputFormat());
                                    break;
                                case MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED:
                                    break;
                                case MediaCodec.INFO_TRY_AGAIN_LATER:
                                    Log.d("GIAMMI", "Try later!");
                                    break;
                                default:
                                    decoder.releaseOutputBuffer(indexOut, true);
                                    break;
                            }
                        } catch (Exception e) {

                        }
                    }

                    @Override
                    public void onError(@NonNull MediaCodec mediaCodec, @NonNull MediaCodec.CodecException e) {

                    }

                    @Override
                    public void onOutputFormatChanged(@NonNull MediaCodec mediaCodec, @NonNull MediaFormat _mediaFormat) {
                        Log.d("GIAMMI", "New format " + decoder.getOutputFormat());
                    }
                });

                decoder.configure(format, surface, null, 0);
                decoder.start();

                int frameCounter = 0;

                final int maxBuffSize = 1920 * 1080;
                byte[] buff = new byte[maxBuffSize];
                byte[] toSend = null;
                int d = 0;
                int tosendOffset = 0;
                int remainingSize = 0;
                int fps= 0;

                long begin = System.currentTimeMillis();

                while ((d = inputStream.read(buff)) >= 0) {

                    boolean searchMore;

                    do {

                        searchMore = false;

                        int toCopyFromBuff = d;
                        int toCopyStart = 0;

                        if (remainingSize <= 0) {
                            byte newbuff[] = new byte[maxBuffSize];
                            while (buff.length < 4 && inputStream.read(newbuff) >= 0) {
                                Log.d(TAG, "ERROREEEE " + buff.length);
                                d += newbuff.length;

                                // create the resultant array
                                byte[] c = new byte[buff.length + newbuff.length];

                                // using the pre-defined function arraycopy
                                System.arraycopy(buff, 0, c, 0, buff.length);
                                System.arraycopy(newbuff, 0, c, buff.length, newbuff.length);
                                buff = c;
                            }

                            toCopyFromBuff = d;
                            int size = ((buff[0] & 0x7f) << 24) + ((buff[1] & 0xff) << 16) + ((buff[2] & 0xff) << 8) + ((buff[3] & 0xff));
                            if (size > maxBuffSize) {
                                Log.d(TAG, "Size troppo grande " + size);
                                Log.d(TAG, "Arr " + buff[0] + " " + buff[1] + " " + buff[2] + " " + buff[3]);
                                return;
                            }
                            if (size < 0) {
                                Log.d(TAG, "Negative size " + size);
                                return;
                            }
                            if (size == 0)
                                continue;
//                            Log.d(TAG, "Size " + size + " -> " + buff[0] + " " + buff[1] + " " + buff[2] + " " + buff[3]);
                            toSend = new byte[size];
                            tosendOffset = 0;
                            remainingSize = size;
                            toCopyFromBuff -= 4;
                            toCopyStart = 4;
                        }

                        int toCopy = Math.min(remainingSize, toCopyFromBuff);

                        // copy all from buff to tosend
                        System.arraycopy(buff, toCopyStart, toSend, tosendOffset, toCopy);

                        remainingSize -= toCopy;
                        tosendOffset += toCopy;
                        d = d - (toCopy + toCopyStart);

                        // display the frame
                        if (remainingSize <= 0) {

                            remainingSize = 0;

                            int inputIndex = -1;
                            try {
                                inputIndex = freeInputs.take();
                            } catch (InterruptedException e) {
                                throw new RuntimeException(e);
                            }

                            if (inputIndex != -1) {
                                ByteBuffer inputBuf = decoder.getInputBuffer(inputIndex);
                                inputBuf.clear();

                                inputBuf.put(toSend);

                                frameCounter++;

                                fps++;

                                if (System.currentTimeMillis() - begin > 1000) {
                                    begin = System.currentTimeMillis();
//                                    Log.d("GIAMMI", "FPS " + fps);
                                    frameRate = fps;
                                    fps = 0;
                                }

                                try {
                                    decoder.queueInputBuffer(inputIndex, 0, toSend.length, 8 * (frameCounter) * (1 / frameRate), 0);
                                } catch (Exception e) {
                                    logExc(e);
                                }
                            }

                            int abundantSize = (toCopyFromBuff) - (toCopy);

                            if (abundantSize > 0) {

                                byte newBuff[] = new byte[abundantSize];
                                System.arraycopy(buff, toCopy + toCopyStart, newBuff, 0, abundantSize);
                                buff = newBuff;

                                searchMore = true;
                            } else {
                                break;
                            }
                        }

                    } while(searchMore);

                    // so that the socket can refill the buffer
                    buff = new byte[1920*1080];

                }

                Log.d("GIAMMI", "Stopping : " + d);
                decoder.stop();
                decoder.release();
            } catch (IOException e2) {
                if (socket != null) {
                    try {
                        socket.close();
                    } catch (IOException e) {
                        logExc(e);
                    }
                }
                Log.d("GIAMMI", "try terminated");
                logExc(e2);
            }

            try {
                socket.close();
            } catch (IOException e) {
                Toast.makeText(getApplicationContext(), "Cannot close socket", Toast.LENGTH_SHORT).show();
            }
        }
    }

}

