#!/bin/bash
#
# 交叉编译 V4L2-Camera-App 的库部分到 ARM，装进 libcamera-arm/。
#
# 为什么是"装成前缀"而不是"把 src/ 拷过来"：
#   上游本来就把库编成 libcamera.a，这本身就是接口声明——消费方要的是
#   .a + 头文件，源码留在它自己的仓库里。把它拷进来是同一份代码存两遍，
#   还只能靠人手同步，分叉了也查不出来（踩过）。
#
# 目录布局（和 jpeg-arm-turbo/ 同构）：
#   libcamera-build/     构建目录（cmake 产物）
#   libcamera-arm/       install prefix：lib/libcamera.a + include/camera/*.h
#   libcamera-arm/VERSION  这次装的是上游哪个 commit
#
# 用法：tools/build_libcamera.sh [--rebuild]
#   上游不在默认位置时用 V4L2_CAMERA_APP=<路径> 指定
#
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT="$PWD"

UPSTREAM="${V4L2_CAMERA_APP:-$HOME/V4L2-Camera-App}"
PREFIX="$ROOT/libcamera-arm"
BUILD_DIR="$ROOT/libcamera-build"
JPEG_PREFIX="$ROOT/jpeg-arm-turbo"

[ -d "$UPSTREAM/src" ] || {
    echo "错误: 找不到上游仓库 $UPSTREAM（用 V4L2_CAMERA_APP=<路径> 指定）" >&2
    exit 1
}
[ -f "$ROOT/toolchain.cmake" ] || { echo "错误: 找不到 toolchain.cmake" >&2; exit 1; }
[ -f "$JPEG_PREFIX/lib/libjpeg.a" ] || {
    echo "错误: 找不到 $JPEG_PREFIX/lib/libjpeg.a，先跑 tools/build_libjpeg_turbo.sh" >&2
    exit 1
}

if [ "${1:-}" != "--rebuild" ] && [ -f "$PREFIX/lib/libcamera.a" ]; then
    echo "已存在 $PREFIX/lib/libcamera.a，跳过（--rebuild 强制重编）"
    exit 0
fi

rm -rf "$BUILD_DIR" "$PREFIX"

echo "[1/2] cmake 配置（只编库，-DBUILD_APPS=OFF 不碰 SDL2）..."
#
# JPEG_INCLUDE_DIR / JPEG_LIBRARY 必须显式给：上游默认走 find_package(JPEG)，
# 那会找到宿主机的 x86 版 libjpeg，链进 ARM 程序里跑不起来。
cmake -S "$UPSTREAM" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/toolchain.cmake" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBUILD_APPS=OFF \
    -DJPEG_INCLUDE_DIR="$JPEG_PREFIX/include" \
    -DJPEG_LIBRARY="$JPEG_PREFIX/lib/libjpeg.a" \
    > "$BUILD_DIR.cmake.log" 2>&1 || {
        echo "cmake 配置失败，最后 30 行：" >&2
        tail -30 "$BUILD_DIR.cmake.log" >&2
        exit 1
    }

echo "[2/2] make + install ..."
cmake --build "$BUILD_DIR" -j"$(nproc)" > "$BUILD_DIR.make.log" 2>&1 || {
    echo "编译失败，最后 30 行：" >&2
    tail -30 "$BUILD_DIR.make.log" >&2
    exit 1
}
cmake --install "$BUILD_DIR" > /dev/null

# 记下装的是上游哪个版本。prefix 被 .gitignore 忽略，不留这个就无从追溯——
# 那样"装了个旧版却没人知道"和当初的源码拷贝是同一个病。
VERSION="unknown"
if git -C "$UPSTREAM" rev-parse --git-dir > /dev/null 2>&1; then
    VERSION="$(git -C "$UPSTREAM" describe --always --dirty 2>/dev/null \
               || git -C "$UPSTREAM" rev-parse HEAD)"
fi
printf '%s\n' "$VERSION" > "$PREFIX/VERSION"

echo
echo "=== 产物 ==="
ls -lh "$PREFIX/lib/libcamera.a"
file "$PREFIX/lib/libcamera.a"
echo "头文件:"
ls "$PREFIX/include/camera/"
echo "上游版本: $VERSION"
