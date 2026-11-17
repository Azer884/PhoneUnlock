#!/usr/bin/env bash
# Builds PhoneUnlock.apk without Gradle: aapt2 -> javac -> d8 -> zipalign -> apksigner.
#
# Needs: JDK 17+, Android SDK with "platforms;android-34" and "build-tools;34.0.0".
# Env:   ANDROID_HOME (default: ~/Android/Sdk or %LOCALAPPDATA%/Android/Sdk)
#        KEYSTORE / KEYSTORE_PASS (default: android/phoneunlock.keystore, created on first run)
# Out:   android/build/PhoneUnlock.apk
set -euo pipefail
cd "$(dirname "$0")"

VERSION_CODE="${VERSION_CODE:-1}"
VERSION_NAME="${VERSION_NAME:-1.0.0}"

to_unix() { if command -v cygpath >/dev/null 2>&1; then cygpath -u "$1"; else echo "$1"; fi; }
to_native() { if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else echo "$1"; fi; }

if [ -z "${ANDROID_HOME:-}" ]; then
  for c in "$HOME/Android/Sdk" "${USERPROFILE:-}/Android/Sdk" "${LOCALAPPDATA:-}/Android/Sdk"; do
    c="$(to_unix "$c")"
    [ -d "$c/platforms/android-34" ] && ANDROID_HOME="$c" && break
  done
fi
SDK="$(to_unix "${ANDROID_HOME:?set ANDROID_HOME}")"
BT="$SDK/build-tools/34.0.0"
AJ="$SDK/platforms/android-34/android.jar"
EXE=""; BAT=""
[ -f "$BT/aapt2.exe" ] && EXE=".exe" && BAT=".bat"

if [ -z "${JAVA_HOME:-}" ] && [ -d "/c/Program Files/Microsoft" ]; then
  JAVA_HOME="$(ls -d "/c/Program Files/Microsoft/jdk-17"* 2>/dev/null | head -1)"
  export JAVA_HOME
fi
[ -n "${JAVA_HOME:-}" ] && export PATH="$JAVA_HOME/bin:$PATH"

KEYSTORE="${KEYSTORE:-phoneunlock.keystore}"
KEYSTORE_PASS="${KEYSTORE_PASS:-phoneunlock}"

rm -rf build
mkdir -p build/gen build/classes build/dex

"$BT/aapt2$EXE" compile --dir res -o build/res.zip
"$BT/aapt2$EXE" link -o build/base.apk -I "$AJ" --manifest AndroidManifest.xml -R build/res.zip \
  --java build/gen --auto-add-overlay \
  --min-sdk-version 26 --target-sdk-version 34 \
  --version-code "$VERSION_CODE" --version-name "$VERSION_NAME"

SEP=":"; [ -n "$EXE" ] && SEP=";"
javac -nowarn -Xlint:-options -source 8 -target 8 -encoding UTF-8 \
  -bootclasspath "$(to_native "$AJ")$SEP$(to_native "$BT/core-lambda-stubs.jar")" \
  -d build/classes $(find src build/gen -name '*.java')

"$BT/d8$BAT" --release --lib "$AJ" --min-api 26 --output build/dex $(find build/classes -name '*.class')

(cd build/dex && "$BT/aapt$EXE" add ../base.apk classes.dex > /dev/null)
"$BT/zipalign$EXE" -f -p 4 build/base.apk build/aligned.apk

if [ ! -f "$KEYSTORE" ]; then
  echo "Creating signing key $KEYSTORE (keep it: updates must be signed with the same key)"
  keytool -genkeypair -keystore "$KEYSTORE" -storepass "$KEYSTORE_PASS" -keypass "$KEYSTORE_PASS" \
    -alias phoneunlock -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Phone Unlock" > /dev/null 2>&1
fi
"$BT/apksigner$BAT" sign --ks "$KEYSTORE" --ks-pass "pass:$KEYSTORE_PASS" --key-pass "pass:$KEYSTORE_PASS" \
  --out build/PhoneUnlock.apk build/aligned.apk
"$BT/apksigner$BAT" verify build/PhoneUnlock.apk
rm -f build/PhoneUnlock.apk.idsig
echo "APK OK: $(to_native "$(pwd)/build/PhoneUnlock.apk")"
