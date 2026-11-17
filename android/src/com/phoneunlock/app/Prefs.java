package com.phoneunlock.app;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Base64;

/** App settings and the shared key (kept in app-private storage). */
final class Prefs {
    private static final String FILE = "phoneunlock";
    private static final String KEY = "key";
    private static final String BT_ADDR = "pc_bt_addr";
    private static final String BT_NAME = "pc_bt_name";
    private static final String IP = "pc_ip";
    private static final String ENABLED = "enabled";

    private Prefs() {}

    private static SharedPreferences p(Context c) {
        return c.getSharedPreferences(FILE, Context.MODE_PRIVATE);
    }

    static byte[] key(Context c) {
        String s = p(c).getString(KEY, "");
        if (s.isEmpty()) return null;
        try {
            byte[] k = Base64.decode(s, Base64.NO_WRAP);
            return k.length == 32 ? k : null;
        } catch (IllegalArgumentException e) {
            return null;
        }
    }

    static boolean isPaired(Context c) {
        return key(c) != null && !btAddress(c).isEmpty();
    }

    static String btAddress(Context c) { return p(c).getString(BT_ADDR, ""); }
    static String btName(Context c) { return p(c).getString(BT_NAME, ""); }
    static String pcIp(Context c) { return p(c).getString(IP, ""); }
    static boolean enabled(Context c) { return p(c).getBoolean(ENABLED, true); }

    static void savePairing(Context c, byte[] key, String addr, String name) {
        p(c).edit()
                .putString(KEY, Base64.encodeToString(key, Base64.NO_WRAP))
                .putString(BT_ADDR, addr)
                .putString(BT_NAME, name)
                .apply();
    }

    static void forget(Context c) {
        p(c).edit().remove(KEY).remove(BT_ADDR).remove(BT_NAME).apply();
    }

    static void setPcIp(Context c, String ip) { p(c).edit().putString(IP, ip).apply(); }
    static void setEnabled(Context c, boolean on) { p(c).edit().putBoolean(ENABLED, on).apply(); }
}
