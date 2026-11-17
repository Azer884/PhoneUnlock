# Phone Unlock

Unlock your Android phone, and your locked Windows PC signs you in.

Phone Unlock adds a sign-in option to the Windows lock screen. When the PC is locked and you unlock your phone,
the phone sends a signed signal to the PC over Bluetooth (or Wi-Fi), and Windows signs you in.
If your phone was already unlocked when the PC locked, nothing happens until you lock and unlock the phone again.

- Works over **Bluetooth**, so it doesn't depend on the network (school, office and public Wi-Fi included)
- Optional **Wi-Fi** path for home networks
- Your PIN, password and fingerprint keep working as usual
- No accounts, no cloud, no tracking

> Not affiliated with Microsoft or Google.

## Download

Get the latest files from [Releases](https://github.com/Azer884/PhoneUnlock/releases):

| File | Where |
|---|---|
| `PhoneUnlockSetup.exe` | Windows 10 / 11 (64-bit) |
| `PhoneUnlock.apk` | Android 8.0 or newer |
| `SHA256SUMS.txt` | Checksums |

## Setup

**Before you start:** pair your phone with the PC in Windows Bluetooth settings (Settings > Bluetooth & devices > Add device).

### On the PC

1. Run `PhoneUnlockSetup.exe` and accept the admin prompt.
   Windows SmartScreen may warn because the app isn't code-signed: click **More info > Run anyway**.
2. Click **Install**. If "Only allow Windows Hello sign-in" is on, the app offers to turn it off.
   This is required, because that setting blocks every non-Hello sign-in method. Your PIN and fingerprint keep working.
3. **Step 1:** type your Windows password and click **Save**.
   For a Microsoft account, this is your Microsoft account password, not your PIN.
4. **Step 2:** click **Pair**. Leave the window open.

### On the phone

5. Install `PhoneUnlock.apk` (allow "Install unknown apps" for your browser or file manager).
6. Open Phone Unlock and allow **Nearby devices** and **Notifications**.
7. Tap **Pair with PC**, pick your PC, and check that the code matches the one on the PC. Tap **Yes, pair**.
8. Tap **Allow background use**, so battery saver doesn't stop the app.

### Test it

9. On the PC, click **Test**, then lock and unlock your phone. You should see "It works".
10. Press **Win+L**, wait a few seconds, then unlock your phone.

You can open the app again later from the Start menu (**Phone Unlock**).

## How it works

```
 Phone                                          PC (lock screen)
 ─────                                          ─────────────────
 you unlock the phone
 ACTION_USER_PRESENT ──► foreground service
                         builds packet:
                         version | time | HMAC-SHA256(key)
                              │
                              ├── Bluetooth RFCOMM ──►  credential provider (LogonUI)
                              └── Wi-Fi UDP 47600 ──►   listening only while locked
                                                        checks HMAC, time window, replay
                                                        ──► Windows signs you in
```

- **PC:** a [credential provider](https://learn.microsoft.com/windows/win32/secauthn/credential-providers-in-windows)
  DLL loaded by the Windows lock screen. It listens on Bluetooth and UDP only while the lock screen is showing.
  When a valid signal arrives, it asks Windows to sign in with your saved password.
- **Phone:** a foreground service catches the "phone unlocked" event and sends the signal for about 3 seconds.
  Because the PC listens only while locked, the signal does nothing at other times.
- **Pairing:** the PC generates a random 256-bit key and sends it to the phone over the bonded Bluetooth link.
  Both screens show a 6-digit code derived from the key, and you confirm it on the phone.

### Security

| | |
|---|---|
| Signal | `version (1) + unix-ms timestamp (8) + HMAC-SHA256(key, first 9 bytes) (32)` |
| Freshness | Rejected if the clock difference is over 60 s, or if the timestamp isn't newer than the last accepted one (replay protection) |
| Password | Stored as an LSA secret: encrypted by Windows, readable only by SYSTEM and administrators |
| Phone key | Stored in the app's private storage |
| Bad password | If Windows rejects the saved password once, Phone Unlock pauses itself, so it can't trigger an account lockout |
| Network | The Wi-Fi firewall rule only allows your local subnet |

**What this means for you:** anyone who can unlock your phone while it's near your PC can unlock your PC.
Use a strong lock on your phone. If you lose the phone, open Phone Unlock on the PC and turn it off or uninstall it.

## Troubleshooting

| Problem | Fix |
|---|---|
| Test says "No signal from the phone" | Check Bluetooth is on for both devices, the phone is paired in Windows Bluetooth settings, and the app shows **Ready**. |
| "Couldn't reach the PC" while pairing | Click **Pair** on the PC first, then tap **Pair with PC** on the phone within 3 minutes. |
| Lock screen says the saved password was rejected | Your password changed. Open Phone Unlock on the PC and save the new password. |
| Works from the Test button but not on the lock screen | Make sure "Only allow Windows Hello sign-in" is off (click **Repair** in the PC app). |
| Stops working after a while | On the phone, allow background use. Some brands (Samsung, Xiaomi, Huawei) also need the app excluded from "sleeping apps". |

## Uninstall

- **PC:** open Phone Unlock and click **Uninstall**, or use Settings > Apps > Installed apps > Phone Unlock.
  This removes the plugin, the saved password and the pairing key.
- **Phone:** uninstall the app as usual.

### If the lock screen ever misbehaves

The normal sign-in options stay available under **Sign-in options** on the lock screen.
If you still can't sign in:

1. On the lock screen, hold **Shift** and click **Power > Restart**.
2. Go to **Troubleshoot > Advanced options > Command Prompt** and run:
   ```
   reg load HKLM\OFF C:\Windows\System32\config\SOFTWARE
   reg delete "HKLM\OFF\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\{7B2E4A91-3C5D-4F8E-9A16-2D4C8B7E5F03}" /f
   reg unload HKLM\OFF
   ```
   In recovery, Windows may be on another drive letter: check with `dir D:\Windows`.
3. Restart.

## Build from source

**Windows** needs Visual Studio 2022 or Build Tools with the "Desktop development with C++" workload.
**Android** needs JDK 17 and the Android SDK (`platforms;android-34`, `build-tools;34.0.0`). Gradle isn't needed.

```powershell
powershell -ExecutionPolicy Bypass -File build_release.ps1 -Version 1.0.0
```

Output goes to `release\`. The separate steps are `windows\build.cmd` and `android/build_apk.sh`.
The APK signing key is created on the first build as `android/phoneunlock.keystore`. Keep it, because app
updates must be signed with the same key. It's git-ignored.

### Layout

```
windows/
  src/dll.cpp, provider.*, credential.*   credential provider (PhoneUnlockCP.dll)
  src/app.cpp, app.rc                     setup app (PhoneUnlockSetup.exe, DLL embedded)
  src/shared.cpp, common.h                packet, secrets, Bluetooth helpers
android/
  src/com/phoneunlock/app/                app, service, pairing, sender
  res/                                    icons, theme
build_release.ps1
```

## Limitations

- One Windows account per PC: the account that ran setup.
- Windows 10/11 64-bit only. The phone app needs Android 8.0 or newer.
- The PC must be awake, so it works on the lock screen but not from sleep or hibernation.
- Not code-signed, so expect SmartScreen and "unknown app" warnings.

## License

[MIT](LICENSE)
