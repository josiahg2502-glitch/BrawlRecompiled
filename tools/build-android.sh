#!/usr/bin/env bash
# Android build of Brawl Recompiled (arm64-v8a): the translated game, its runtime, SDL and Dawn as one libmain.so in
# the app in wiicompiled/android. Run on Linux after the game has been translated (wiicompiled/generated: the
# translate, data-init and shards steps of tools/build-brawl.ps1, which run on Linux too with dotnet).
#   bash tools/build-android.sh [--jobs N] [--skip-native] [--install]
# Needs the Android SDK with an NDK (ANDROID_HOME, default ~/Android/Sdk), JDK 17 (JAVA_HOME) and a Gradle 8.7+
# (GRADLE, default: the newest in ~/.gradle/wrapper/dists). Output: wiicompiled/android/app/build/outputs/apk/release.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
wc=$root/wiicompiled
jobs=8
translated_jobs=4   # a translated shard can take ~2.5 GB to compile
native=1
install=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) jobs=$2; shift ;;
        --skip-native) native=0 ;;
        --install) install=1 ;;
        *) echo "unknown argument $1" >&2; exit 1 ;;
    esac
    shift
done

sdk=${ANDROID_HOME:-$HOME/Android/Sdk}
ndk=${ANDROID_NDK_HOME:-$(ls -d "$sdk"/ndk/* | sort -V | tail -1)}
build=${BRAWL_ANDROID_BUILD:-$HOME/brawl-android/build}

if [[ $native == 1 ]]; then
    [[ -f $wc/generated/build_shards/shards.cmake ]] || { echo "translate the game first (wiicompiled/generated)" >&2; exit 1; }
    # Dawn comes as the pinned prebuilt android-aarch64 package (there is no source tarball for its version).
    cmake -S "$wc/runtime" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-30 -DAURORA_DAWN_PROVIDER=package \
        -DMKW_TRANSLATED_COMPILE_JOBS="$translated_jobs"
    cmake --build "$build" --target WiiCompiled --parallel "$jobs"
fi

# Stage libmain.so and the files a desktop build keeps beside its executable (AppFiles.prepareRuntime unpacks them).
stage=$wc/android/native
rm -rf "$stage"
mkdir -p "$stage/jniLibs/arm64-v8a" "$stage/assets/runtime"
"$ndk"/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-debug -o "$stage/jniLibs/arm64-v8a/libmain.so" "$build/libmain.so"
for f in dsp_coef.bin initial_pipeline_cache.db cacert.pem wii_bootstrap; do
    cp -r "$build/$f" "$stage/assets/runtime/"
done

gradle=${GRADLE:-$(ls -d "$HOME"/.gradle/wrapper/dists/gradle-*/*/gradle-*/bin/gradle 2>/dev/null | sort -V | tail -1)}
[[ -x $gradle ]] || { echo "no Gradle found: set GRADLE" >&2; exit 1; }
echo "sdk.dir=$sdk" > "$wc/android/local.properties"
(cd "$wc/android" && "$gradle" --no-daemon -q assembleRelease)
apk=$wc/android/app/build/outputs/apk/release/app-release.apk
echo "DONE: $apk"
if [[ $install == 1 ]]; then
    adb install -r "$apk"
fi
