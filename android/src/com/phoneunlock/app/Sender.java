package com.phoneunlock.app;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothSocket;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.util.Log;

import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.util.UUID;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/** Builds and sends the signed "phone was just unlocked" signal over Bluetooth and Wi-Fi. */
final class Sender {
    static final String TAG = "PhoneUnlock";
    static final int UDP_PORT = 47600;

    /** Must match PU_BT_UNLOCK_UUID / PU_BT_PAIR_UUID on the PC. */
    static final UUID BT_UNLOCK_UUID = UUID.fromString("6b1f3a2e-8d4c-4b7a-9e2f-5c3d1a0b7e64");
    static final UUID BT_PAIR_UUID = UUID.fromString("6b1f3a2e-8d4c-4b7a-9e2f-5c3d1a0b7e65");

    private Sender() {}

    static boolean hasBtPermission(Context ctx) {
        return Build.VERSION.SDK_INT < 31
                || ctx.checkSelfPermission("android.permission.BLUETOOTH_CONNECT") == PackageManager.PERMISSION_GRANTED;
    }

    static BluetoothAdapter adapter(Context ctx) {
        BluetoothManager bm = ctx.getSystemService(BluetoothManager.class);
        return bm == null ? null : bm.getAdapter();
    }

    /** Packet: [0]=version, [1..8]=unix ms big endian, [9..40]=HMAC-SHA256(key, bytes 0..8). */
    static byte[] buildPacket(byte[] key) throws Exception {
        byte[] pkt = new byte[41];
        pkt[0] = 1;
        long ts = System.currentTimeMillis();
        for (int i = 0; i < 8; i++) {
            pkt[1 + i] = (byte) (ts >>> (56 - 8 * i));
        }
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(key, "HmacSHA256"));
        mac.update(pkt, 0, 9);
        System.arraycopy(mac.doFinal(), 0, pkt, 9, 32);
        return pkt;
    }

    /** Wi-Fi: broadcast + optional PC IP. Only works on networks that let devices talk to each other. */
    static void trySendWifi(Context ctx) {
        byte[] key = Prefs.key(ctx);
        if (key == null) return;
        try {
            byte[] pkt = buildPacket(key);
            DatagramSocket s = new DatagramSocket();
            try {
                s.setBroadcast(true);
                sendTo(s, pkt, "255.255.255.255");
                String ip = Prefs.pcIp(ctx).trim();
                if (!ip.isEmpty()) sendTo(s, pkt, ip);
            } finally {
                s.close();
            }
        } catch (Exception e) {
            Log.w(TAG, "wifi send failed: " + e);
        }
    }

    private static void sendTo(DatagramSocket s, byte[] pkt, String host) {
        try {
            s.send(new DatagramPacket(pkt, pkt.length, InetAddress.getByName(host), UDP_PORT));
        } catch (Exception e) {
            Log.w(TAG, "send to " + host + " failed: " + e);
        }
    }

    /**
     * Bluetooth: connect to the PC's RFCOMM service and send one packet.
     * Returns true only if the PC answered "accepted". The PC only listens while it is locked.
     */
    static boolean trySendBluetooth(Context ctx) {
        byte[] key = Prefs.key(ctx);
        String addr = Prefs.btAddress(ctx);
        BluetoothAdapter adapter = adapter(ctx);
        if (key == null || addr.isEmpty() || !hasBtPermission(ctx) || adapter == null || !adapter.isEnabled()) {
            return false;
        }
        BluetoothSocket sock = null;
        try {
            BluetoothDevice dev = adapter.getRemoteDevice(addr);
            sock = dev.createRfcommSocketToServiceRecord(BT_UNLOCK_UUID);
            sock.connect();
            OutputStream out = sock.getOutputStream();
            out.write(buildPacket(key));
            out.flush();
            InputStream in = sock.getInputStream();
            return in.read() == 1;
        } catch (Exception e) {
            Log.i(TAG, "bluetooth send: " + e);
            return false;
        } finally {
            if (sock != null) {
                try {
                    sock.close();
                } catch (Exception ignored) {
                }
            }
        }
    }
}
