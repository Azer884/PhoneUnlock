package com.phoneunlock.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.content.Intent;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.provider.Settings;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.List;

public class MainActivity extends Activity {
    private static final String GITHUB = "https://github.com/Azer884/PhoneUnlock";

    private float dp;
    private TextView statusTitle;
    private TextView statusDetail;
    private Button pairButton;
    private Button forgetButton;
    private Switch enabledSwitch;
    private Button testButton;
    private LinearLayout batteryRow;
    private volatile boolean pairingCancelled;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        dp = getResources().getDisplayMetrics().density;

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(px(20), px(32), px(20), px(24));

        TextView title = text("Phone Unlock", 28, true);
        root.addView(title);
        TextView subtitle = text("Unlock your phone, and your locked PC signs you in.", 15, false);
        subtitle.setAlpha(0.7f);
        subtitle.setPadding(0, px(4), 0, px(16));
        root.addView(subtitle);

        // Status
        LinearLayout status = card(root);
        statusTitle = text("", 20, true);
        status.addView(statusTitle);
        statusDetail = text("", 14, false);
        statusDetail.setAlpha(0.75f);
        statusDetail.setPadding(0, px(4), 0, 0);
        status.addView(statusDetail);

        // Pairing
        LinearLayout pair = card(root);
        pair.addView(heading("Pair with your PC"));
        pair.addView(body("1. Pair this phone with the PC in Bluetooth settings.\n"
                + "2. On the PC, open Phone Unlock and click Pair.\n"
                + "3. Tap the button below and check the codes match."));
        pairButton = button("Pair with PC", v -> startPairing());
        pair.addView(pairButton);
        forgetButton = button("Forget this PC", v -> confirmForget());
        forgetButton.setBackground(null);
        pair.addView(forgetButton);

        // Settings
        LinearLayout settings = card(root);
        settings.addView(heading("Settings"));
        enabledSwitch = new Switch(this);
        enabledSwitch.setText("Unlock my PC when I unlock this phone");
        enabledSwitch.setTextSize(15);
        enabledSwitch.setPadding(0, px(8), 0, px(8));
        enabledSwitch.setOnCheckedChangeListener((b, on) -> {
            Prefs.setEnabled(this, on);
            UnlockService.sync(this);
            refresh();
        });
        settings.addView(enabledSwitch);
        testButton = button("Send test signal", v -> {
            startForegroundService(new Intent(this, UnlockService.class).setAction(UnlockService.ACTION_TEST));
            Toast.makeText(this, "Sent. On the PC, Test shows the result.", Toast.LENGTH_LONG).show();
        });
        settings.addView(testButton);

        batteryRow = new LinearLayout(this);
        batteryRow.setOrientation(LinearLayout.VERTICAL);
        batteryRow.setPadding(0, px(12), 0, 0);
        TextView batteryText = body("Battery saver may stop Phone Unlock in the background.");
        batteryRow.addView(batteryText);
        batteryRow.addView(button("Allow background use", v -> requestBatteryExemption()));
        settings.addView(batteryRow);

        // Wi-Fi (optional)
        LinearLayout wifi = card(root);
        wifi.addView(heading("Wi-Fi (optional)"));
        wifi.addView(body("Bluetooth is used first. On a home network the phone can also reach the PC over Wi-Fi. "
                + "Enter the PC's IP address to help it find the PC."));
        EditText ip = new EditText(this);
        ip.setHint("e.g. 192.168.1.20");
        ip.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        ip.setText(Prefs.pcIp(this));
        wifi.addView(ip, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        wifi.addView(button("Save", v -> {
            Prefs.setPcIp(this, ip.getText().toString().trim());
            Toast.makeText(this, "Saved", Toast.LENGTH_SHORT).show();
        }));

        // Footer
        TextView footer = text("Version " + versionName() + "  ·  GitHub", 13, false);
        footer.setAlpha(0.6f);
        footer.setGravity(Gravity.CENTER);
        footer.setPadding(0, px(16), 0, 0);
        footer.setOnClickListener(v -> startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(GITHUB))));
        root.addView(footer);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);

        List<String> perms = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= 31) perms.add("android.permission.BLUETOOTH_CONNECT");
        if (Build.VERSION.SDK_INT >= 33) perms.add("android.permission.POST_NOTIFICATIONS");
        if (!perms.isEmpty()) requestPermissions(perms.toArray(new String[0]), 1);

        UnlockService.sync(this);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    private void refresh() {
        boolean paired = Prefs.isPaired(this);
        boolean on = Prefs.enabled(this);
        boolean bt = Sender.hasBtPermission(this);

        if (!paired) {
            statusTitle.setText("Not paired");
            statusDetail.setText("Pair with your PC to get started.");
        } else if (!bt) {
            statusTitle.setText("Needs permission");
            statusDetail.setText("Allow \"Nearby devices\" for Phone Unlock in the app settings.");
        } else if (!on) {
            statusTitle.setText("Turned off");
            statusDetail.setText("Paired with " + Prefs.btName(this) + ".");
        } else {
            statusTitle.setText("Ready");
            statusDetail.setText("Paired with " + Prefs.btName(this)
                    + ". When it's locked, unlocking this phone signs you in.");
        }

        pairButton.setText(paired ? "Pair again" : "Pair with PC");
        forgetButton.setVisibility(paired ? View.VISIBLE : View.GONE);
        enabledSwitch.setEnabled(paired);
        enabledSwitch.setChecked(on);
        testButton.setEnabled(paired && on);

        PowerManager pm = getSystemService(PowerManager.class);
        batteryRow.setVisibility(pm.isIgnoringBatteryOptimizations(getPackageName()) ? View.GONE : View.VISIBLE);
    }

    // ------------------------------------------------------------------ pairing

    private void startPairing() {
        if (!Sender.hasBtPermission(this)) {
            requestPermissions(new String[]{"android.permission.BLUETOOTH_CONNECT"}, 2);
            Toast.makeText(this, "Allow Nearby devices, then tap again", Toast.LENGTH_LONG).show();
            return;
        }
        BluetoothAdapter adapter = Sender.adapter(this);
        if (adapter == null || !adapter.isEnabled()) {
            Toast.makeText(this, "Turn Bluetooth on first", Toast.LENGTH_LONG).show();
            return;
        }
        final List<BluetoothDevice> devices = new ArrayList<>(adapter.getBondedDevices());
        if (devices.isEmpty()) {
            new AlertDialog.Builder(this)
                    .setTitle("No paired devices")
                    .setMessage("Pair this phone with your PC in Bluetooth settings first.")
                    .setPositiveButton("Open Bluetooth settings",
                            (d, w) -> startActivity(new Intent(Settings.ACTION_BLUETOOTH_SETTINGS)))
                    .setNegativeButton("Cancel", null)
                    .show();
            return;
        }
        String[] names = new String[devices.size()];
        for (int i = 0; i < names.length; i++) {
            String n = devices.get(i).getName();
            names[i] = n == null ? devices.get(i).getAddress() : n;
        }
        new AlertDialog.Builder(this)
                .setTitle("Which device is your PC?")
                .setItems(names, (d, which) -> connect(devices.get(which), names[which]))
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void connect(BluetoothDevice pc, String name) {
        pairingCancelled = false;
        AlertDialog progress = new AlertDialog.Builder(this)
                .setTitle("Connecting to " + name)
                .setMessage("Make sure you clicked Pair in Phone Unlock on the PC.")
                .setCancelable(false)
                .setNegativeButton("Cancel", (d, w) -> pairingCancelled = true)
                .show();

        new Thread(() -> {
            try {
                Pairing p = Pairing.connect(pc);
                runOnUiThread(() -> {
                    progress.dismiss();
                    if (pairingCancelled) {
                        new Thread(() -> p.finish(false)).start();
                        return;
                    }
                    confirmCode(p, pc, name);
                });
            } catch (Exception e) {
                runOnUiThread(() -> {
                    progress.dismiss();
                    if (pairingCancelled) return;
                    new AlertDialog.Builder(this)
                            .setTitle("Couldn't reach the PC")
                            .setMessage("On the PC, open Phone Unlock and click Pair, then try again. "
                                    + "Bluetooth must be on and the devices paired.")
                            .setPositiveButton("OK", null)
                            .show();
                });
            }
        }).start();
    }

    private void confirmCode(Pairing p, BluetoothDevice pc, String name) {
        TextView code = text(p.code, 34, true);
        code.setGravity(Gravity.CENTER);
        code.setPadding(0, px(16), 0, px(8));
        new AlertDialog.Builder(this)
                .setTitle("Does your PC show this code?")
                .setView(code)
                .setCancelable(false)
                .setPositiveButton("Yes, pair", (d, w) -> {
                    Prefs.savePairing(this, p.key, pc.getAddress(), name);
                    Prefs.setEnabled(this, true);
                    new Thread(() -> p.finish(true)).start();
                    UnlockService.sync(this);
                    refresh();
                    Toast.makeText(this, "Paired with " + name, Toast.LENGTH_LONG).show();
                })
                .setNegativeButton("No", (d, w) -> new Thread(() -> p.finish(false)).start())
                .show();
    }

    private void confirmForget() {
        new AlertDialog.Builder(this)
                .setTitle("Forget " + Prefs.btName(this) + "?")
                .setMessage("This phone will stop unlocking it until you pair again.")
                .setPositiveButton("Forget", (d, w) -> {
                    Prefs.forget(this);
                    UnlockService.sync(this);
                    refresh();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    // ------------------------------------------------------------------ misc

    private void requestBatteryExemption() {
        try {
            startActivity(new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                    Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS));
        }
    }

    private String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "?";
        }
    }

    private int px(int v) {
        return (int) (v * dp);
    }

    private TextView text(String s, int sp, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        if (bold) t.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return t;
    }

    private TextView heading(String s) {
        TextView t = text(s, 17, true);
        t.setPadding(0, 0, 0, px(6));
        return t;
    }

    private TextView body(String s) {
        TextView t = text(s, 14, false);
        t.setAlpha(0.8f);
        t.setPadding(0, 0, 0, px(8));
        return t;
    }

    private Button button(String s, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        return b;
    }

    private LinearLayout card(LinearLayout parent) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(px(16), px(16), px(16), px(16));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0x1A808080);
        bg.setCornerRadius(px(16));
        c.setBackground(bg);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = px(12);
        parent.addView(c, lp);
        return c;
    }
}
