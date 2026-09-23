#!/bin/bash
# Standalone cross-build of Mesa's guest Venus Vulkan ICD (vulkan.virtio.so)
# against the Android NDK, mirroring waydroid-nvidia's build/mesa/build.sh.
# Needed because Soong (AOSP's own build system) has no Android.bp for this
# component in this tree - it only ships as a prebuilt.
#
# Usage: build.sh SRCDIR [BUILDDIR]
# Env:   NDK (default /opt/android-ndk), NDK_PKGCONFIG (default: system pkg-config),
#        ANDROID_ABI (x86_64|x86, default x86_64), ANDROID_API (default 34)
set -eu

SRCDIR=$1
BUILDDIR=${2:-/tmp/mesa-build-$ANDROID_ABI}

NDK=${NDK:-/opt/android-ndk}
NDK_PKGCONFIG=${NDK_PKGCONFIG:-/usr/bin/pkg-config}
ANDROID_ABI=${ANDROID_ABI:-x86_64}
ANDROID_API=${ANDROID_API:-34}

case "$ANDROID_ABI" in
    x86_64)
        TARGET=x86_64-linux-android
        CPU_FAMILY=x86_64
        CPU=x86_64
        ;;
    x86)
        TARGET=i686-linux-android
        CPU_FAMILY=x86
        CPU=i686
        ;;
    *)
        echo "unsupported ANDROID_ABI: $ANDROID_ABI" >&2
        exit 1
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CROSS_FILE="$BUILDDIR-cross.ini"
mkdir -p "$BUILDDIR"

sed \
    -e "s#@NDK@#$NDK#g" \
    -e "s#@NDK_PKGCONFIG@#$NDK_PKGCONFIG#g" \
    -e "s#@TARGET@#$TARGET#g" \
    -e "s#@ANDROID_API@#$ANDROID_API#g" \
    -e "s#@CPU_FAMILY@#$CPU_FAMILY#g" \
    -e "s#@CPU@#$CPU#g" \
    "$SCRIPT_DIR/android-cross.ini.in" > "$CROSS_FILE"

if [ ! -f "$BUILDDIR/build.ninja" ]; then
    meson setup "$BUILDDIR" "$SRCDIR" \
        --cross-file "$CROSS_FILE" \
        -Dplatforms=android \
        -Dandroid-stub=true \
        -Dandroid-libbacktrace=disabled \
        -Dvulkan-drivers=virtio \
        -Dgallium-drivers= \
        -Dshared-glapi=disabled \
        -Dgles1=disabled \
        -Dgles2=disabled \
        -Degl=disabled \
        -Dopengl=false \
        -Dplatform-sdk-version="$ANDROID_API" \
        -Dforce_fallback_for=libdrm
fi

ninja -C "$BUILDDIR" src/virtio/vulkan/libvulkan_virtio.so
