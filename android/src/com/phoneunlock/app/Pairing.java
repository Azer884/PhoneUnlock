package com.phoneunlock.app;

import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothSocket;

import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;

/**
 * One-time pairing over the already bonded Bluetooth link.
 * PC sends "PUP1" + 32-byte key; both sides show a 6-digit code derived from the key;
 * the phone answers 1 (user confirmed) or 0.
 */
final class Pairing {
    private final BluetoothSocket socket;
    final byte[] key;
    final String code;

    private Pairing(BluetoothSocket socket, byte[] key) throws Exception {
        this.socket = socket;
        this.key = key;
        this.code = codeFor(key);
    }

    /** Blocking: connects to the PC and receives the key. Call off the UI thread. */
    static Pairing connect(BluetoothDevice pc) throws Exception {
        BluetoothSocket s = pc.createRfcommSocketToServiceRecord(Sender.BT_PAIR_UUID);
        try {
            s.connect();
            byte[] msg = readFully(s.getInputStream(), 36);
            if (msg[0] != 'P' || msg[1] != 'U' || msg[2] != 'P' || msg[3] != '1') {
                throw new IOException("unexpected reply from PC");
            }
            byte[] key = new byte[32];
            System.arraycopy(msg, 4, key, 0, 32);
            return new Pairing(s, key);
        } catch (Exception e) {
            try {
                s.close();
            } catch (IOException ignored) {
            }
            throw e;
        }
    }

    /** Blocking: tells the PC whether the user confirmed the code, then closes. */
    void finish(boolean confirmed) {
        try {
            socket.getOutputStream().write(confirmed ? 1 : 0);
            socket.getOutputStream().flush();
            Thread.sleep(300);
        } catch (Exception ignored) {
        } finally {
            try {
                socket.close();
            } catch (IOException ignored) {
            }
        }
    }

    /** Same formula as PU_PairCode on the PC: first 4 bytes of SHA-256(key), mod 1,000,000. */
    static String codeFor(byte[] key) throws Exception {
        byte[] h = MessageDigest.getInstance("SHA-256").digest(key);
        long v = ((long) (h[0] & 0xff) << 24) | ((h[1] & 0xff) << 16) | ((h[2] & 0xff) << 8) | (h[3] & 0xff);
        String s = String.format("%06d", v % 1000000L);
        return s.substring(0, 3) + " " + s.substring(3);
    }

    private static byte[] readFully(InputStream in, int n) throws IOException {
        byte[] b = new byte[n];
        int got = 0;
        while (got < n) {
            int r = in.read(b, got, n - got);
            if (r < 0) throw new IOException("PC closed the connection");
            got += r;
        }
        return b;
    }
}
