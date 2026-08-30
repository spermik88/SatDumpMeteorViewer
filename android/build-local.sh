#!/usr/bin/env bash

# Build a debug APK locally on Linux/WSL. The native dependency build is
# intentionally cached in android/deps/output, so it runs only when the
# prebuilt ARM libraries are missing.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

if [[ -z "${ANDROID_HOME:-}" && -d /opt/android-sdk ]]; then
    export ANDROID_HOME=/opt/android-sdk
fi
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"

if [[ -z "${ANDROID_HOME:-}" || ! -x "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" ]]; then
    echo "ANDROID_HOME must point to an Android SDK with Command-line Tools installed." >&2
    exit 1
fi

for required_path in \
    "$ANDROID_HOME/ndk/25.2.9519653" \
    "$ANDROID_HOME/platforms/android-34" \
    "$ANDROID_HOME/cmake/3.22.1"; do
    if [[ ! -d "$required_path" ]]; then
        echo "Missing Android SDK package: $required_path" >&2
        exit 1
    fi
done

if [[ ! -d android/deps/output/arm64-v8a/lib || ! -d android/deps/output/armeabi-v7a/lib ]]; then
    echo "Building cached ARM native dependencies (first run only)..."
    deps_script="$(mktemp)"
    trap 'rm -f "$deps_script"' EXIT
    cp android/deps/build.sh "$deps_script"

    # The viewer ships ARM libraries only. Its historical SQLite clone is no
    # longer public and is not linked by this application.
    sed -i -E \
        -e '/^build_[a-z0-9_]+ (x86|x86_64)$/d' \
        -e '/^git clone https:\/\/github\.com\/R1NC\/sqlite /d' \
        -e '/^build_sqlite [a-z0-9_-]+$/d' \
        -e '/^rm -rf sqlite$/d' \
        "$deps_script"
    (cd android/deps && bash -e "$deps_script")
fi

cd android
./gradlew :app:assembleDebug "$@"
echo "APK: $repo_root/android/app/build/outputs/apk/debug/app-debug.apk"
