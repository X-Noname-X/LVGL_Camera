#!/bin/bash
#
# 交叉编译 libjpeg 到 ARM（静态库），供 MJPEG 解码使用。
#
# 为什么需要：
#   宿主机 /usr/lib/x86_64-linux-gnu/libjpeg.a 是 x86-64 的，链进 ARM 程序
#   要么直接报架构不匹配，要么侥幸链上后板子跑不起来。必须自己编一份。
#   （decoder.c 里 #include <jpeglib.h> 是源码依赖，与架构无关；
#     缺的是 ARM 版的库文件，两者不矛盾。）
#
# 目录布局（按约定放在项目根下）：
#   jpegsrc.v9b.tar.gz   源码包
#   jpeg-9b/             解压出来的源码，就地编译
#   jpeg-arm/            install prefix，ARM 产物（include/ + lib/）
#
# 用法：tools/build_libjpeg.sh [--rebuild]
#
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT="$PWD"

SRC_TARBALL="$ROOT/jpegsrc.v9b.tar.gz"
SRC_DIR="$ROOT/jpeg-9b"
PREFIX="$ROOT/jpeg-arm"
TC_ROOT="$ROOT/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf"

[ -f "$SRC_TARBALL" ] || { echo "错误: 找不到 $SRC_TARBALL" >&2; exit 1; }
[ -x "$TC_ROOT/bin/arm-linux-gnueabihf-gcc" ] || {
    echo "错误: 工具链不在 $TC_ROOT" >&2; exit 1; }

# 已编好就跳过
if [ "${1:-}" != "--rebuild" ] && [ -f "$PREFIX/lib/libjpeg.a" ]; then
    echo "已存在 $PREFIX/lib/libjpeg.a，跳过（--rebuild 强制重编）"
    exit 0
fi

echo "[1/3] 解压 $SRC_TARBALL ..."
rm -rf "$SRC_DIR"
tar -xzf "$SRC_TARBALL" -C "$ROOT"

cd "$SRC_DIR"

echo "[2/3] configure（target=arm-linux-gnueabihf, prefix=$PREFIX）..."
# 工具链加进 PATH，configure 自己会找 arm-linux-gnueabihf-gcc
export PATH="$TC_ROOT/bin:$PATH"
CC=arm-linux-gnueabihf-gcc \
AR=arm-linux-gnueabihf-ar \
RANLIB=arm-linux-gnueabihf-ranlib \
./configure \
    --host=arm-linux-gnueabihf \
    --prefix="$PREFIX" \
    --enable-static \
    --disable-shared \
    --disable-dependency-tracking \
    > /dev/null

echo "[3/3] make + install ..."
make -j"$(nproc)" > /dev/null
make install > /dev/null

echo
echo "=== 产物 ==="
ls -lh "$PREFIX/lib/libjpeg.a" "$PREFIX/include/jpeglib.h"
echo
echo "=== 架构确认（必须是 ARM，不能是 x86-64）==="
file "$PREFIX/lib/libjpeg.a"
ar p "$PREFIX/lib/libjpeg.a" jcapimin.o 2>/dev/null | file - | sed 's/^/  成员 jcapimin.o: /'
