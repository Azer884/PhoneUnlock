package com.phoneunlock.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;

/**
 * Foreground service that listens for ACTION_USER_PRESENT (the phone was just unlocked)
 * and sends a short burst of signed signals to the PC. The PC only listens while its
 * lock screen is showing, so a phone unlock while the PC is already unlocked does nothing.
 */
public class UnlockService extends Service {
    static final String ACTION_TEST = "com.phoneunlock.app.TEST";
    private static final String CHANNEL = "phoneunlock";

    // Wi-Fi can take a moment to wake after the screen turns on, so resend a few times.
    private static final long[] WIFI_DELAYS_MS = {0, 300, 800, 1500, 2500};
    // Bluetooth: a few connect attempts right after the unlock, stop on success.
    private static final int BT_ATTEMPTS = 3;
    private static final long BT_RETRY_MS = 1000;

    private HandlerThread wifiThread;
    private Handler wifi;
    private HandlerThread btThread;
    private Handler bt;

    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (Intent.ACTION_USER_PRESENT.equals(intent.getAction())) {
                burst();
            }
        }
    };

    static void sync(Context ctx) {
        Intent i = new Intent(ctx, UnlockService.class);
        if (Prefs.isPaired(ctx) && Prefs.enabled(ctx)) {
            ctx.startForegroundService(i);
        } else {
            ctx.stopService(i);
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();

        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Phone Unlock", NotificationManager.IMPORTANCE_MIN));
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                PendingIntent.FLAG_IMMUTABLE);
        String pc = Prefs.btName(this);
        Notification n = new Notification.Builder(this, CHANNEL)
                .setContentTitle("Phone Unlock is on")
                .setContentText(pc.isEmpty() ? "Unlocking this phone unlocks your PC" : "Unlocking this phone unlocks " + pc)
                .setSmallIcon(R.drawable.ic_stat)
                .setContentIntent(open)
                .setOngoing(true)
                .build();
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(1, n);
        }

        wifiThread = new HandlerThread("phoneunlock-wifi");
        wifiThread.start();
        wifi = new Handler(wifiThread.getLooper());
        btThread = new HandlerThread("phoneunlock-bt");
        btThread.start();
        bt = new Handler(btThread.getLooper());

        IntentFilter filter = new IntentFilter(Intent.ACTION_USER_PRESENT);
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED);
        } else {
            registerReceiver(receiver, filter);
        }
    }

    private void burst() {
        wifi.removeCallbacksAndMessages(null);
        for (long d : WIFI_DELAYS_MS) {
            wifi.postDelayed(() -> Sender.trySendWifi(this), d);
        }
        bt.removeCallbacksAndMessages(null);
        bt.post(() -> btAttempt(1));
    }

    private void btAttempt(int n) {
        if (Sender.trySendBluetooth(this) || n >= BT_ATTEMPTS) {
            return;
        }
        bt.postDelayed(() -> btAttempt(n + 1), BT_RETRY_MS);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (!Prefs.isPaired(this) || !Prefs.enabled(this)) {
            stopSelf();
            return START_NOT_STICKY;
        }
        if (intent != null && ACTION_TEST.equals(intent.getAction())) {
            burst();
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        try {
            unregisterReceiver(receiver);
        } catch (Exception ignored) {
        }
        if (wifiThread != null) wifiThread.quitSafely();
        if (btThread != null) btThread.quitSafely();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
