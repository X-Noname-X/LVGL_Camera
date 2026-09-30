#!/bin/bash
#
# 交叉编译 libjpeg-turbo 到 ARM（静态库），启用 NEON。
#
# 为什么需要它：
#   IJG 原版 jpeg-9b 是纯 C 零 SIMD，实测在 792MHz 的 Cortex-A7 上解
#   640x360 MJPEG 要 67ms（约 3.5 Mpixel/s），只能跑到 14fps。
#   而 Cortex-A7 有 NEON（/proc/cpuinfo 的 Features 已确认），
#   libjpeg-turbo 的 NEON 路径在这个核上通常快 2~4 倍。
#
# 目录布局（沿用 libjpeg 的约定）：
#   libjpeg-turbo-3.0.4.tar.gz   源码包
#   libjpeg-turbo-3.0.4/         解压出来的源码
#   libjpeg-turbo-build/         构建目录（cmake 产物）
#   jpeg-arm-turbo/              install prefix，ARM 产物
#
# 用法：tools/build_libjpeg_turbo.sh [--rebuild]
#
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT="$PWD"

VER=3.0.4
SRC_TARBALL="$ROOT/libjpeg-turbo-$VER.tar.gz"
SRC_DIR="$ROOT/libjpeg-turbo-$VER"
BUILD_DIR="$ROOT/libjpeg-turbo-build"
PREFIX="$ROOT/jpeg-arm-turbo"

[ -f "$SRC_TARBALL" ] || { echo "错误: 找不到 $SRC_TARBALL" >&2; exit 1; }
[ -f "$ROOT/toolchain.cmake" ] || { echo "错误: 找不到 toolchain.cmake" >&2; exit 1; }

if [ "${1:-}" != "--rebuild" ] && [ -f "$PREFIX/lib/libjpeg.a" ]; then
    echo "已存在 $PREFIX/lib/libjpeg.a，跳过（--rebuild 强制重编）"
    exit 0
fi

echo "[1/3] 解压 ..."
[ -d "$SRC_DIR" ] || tar -xzf "$SRC_TARBALL" -C "$ROOT"
rm -rf "$BUILD_DIR"

echo "[2/3] cmake 配置（开 NEON）..."
#
# -mcpu=cortex-a7 -mfpu=neon 是这里的关键：
#   显式打开 NEON 后，libjpeg-turbo 就不再需要运行时解析 /proc/cpuinfo 探测，
#   也就不必为兼容而把标量部分编译成 soft-float。
#   我们整个程序是 hard-float，ABI 必须一致——测试已确认它不会强加 softfp。
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/toolchain.cmake" \
    -DCMAKE_C_FLAGS="-mcpu=cortex-a7 -mfpu=neon -O2" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DWITH_SIMD=1 \
    -DENABLE_SHARED=0 \
    -DENABLE_STATIC=1 \
    -DWITH_TURBOJPEG=0 \
    -DWITH_12BIT=0 \
    -DWITH_16BIT=0 \
    > "$BUILD_DIR.cmake.log" 2>&1 || {
        echo "cmake 配置失败，最后 30 行：" >&2
        tail -30 "$BUILD_DIR.cmake.log" >&2
        exit 1
    }

echo "  --- cmake 关于 SIMD 的结论 ---"
grep -iE "SIMD extensions|NEON|GAS" "$BUILD_DIR.cmake.log" | sed 's/^/  /' || true

echo "[3/3] make + install ..."
cmake --build "$BUILD_DIR" -j"$(nproc)" > "$BUILD_DIR.make.log" 2>&1 || {
    echo "编译失败，最后 30 行：" >&2
    tail -30 "$BUILD_DIR.make.log" >&2
    exit 1
}
cmake --install "$BUILD_DIR" > /dev/null

echo
echo "=== 产物 ==="
ls -lh "$PREFIX/lib/libjpeg.a"
file "$PREFIX/lib/libjpeg.a"
