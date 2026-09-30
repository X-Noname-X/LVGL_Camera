# 采集层：V4L2 采集 + 有界环形队列 + 解码，编成 libcamera.a
#
# 来源：从 ~/V4L2-Camera-App 拷贝而来（capture / decoder / frame_queue），
# 那份代码本身没改，上游在那里。改动请同步两边。
# 它原项目的 CMakeLists 同时依赖 SDL2 和宿主机的 libjpeg（那是给 PC 上的
# 预览窗口用的），这里不用那份，只取库的部分。

add_library(camera STATIC
    frame_queue.c
    decoder.c
    capture.c
)

target_include_directories(camera PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(Threads REQUIRED)
target_link_libraries(camera PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# libjpeg：必须用我们自己交叉编译的 ARM 版
#
# 这里刻意不用 find_package(JPEG)——那会找到宿主机的
# /usr/lib/x86_64-linux-gnu/libjpeg.a，链进去要么报架构不匹配，
# 要么侥幸通过后在板子上跑不起来。
#
# ARM 版由 tools/build_libjpeg.sh 生成到 jpeg-arm/。
# ---------------------------------------------------------------------------
# 默认用 libjpeg-turbo（带 NEON）。实测 IJG 原版 jpeg-9b 在 792MHz 的
# Cortex-A7 上解 640x360 要 67ms，只能到 14fps；turbo 的 NEON 路径快得多。
#
# 想切回 IJG 原版做对比：
#   cmake -DJPEG_ARM_DIR=$PWD/jpeg-arm -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake -B build
set(JPEG_ARM_DIR "${CMAKE_SOURCE_DIR}/jpeg-arm-turbo" CACHE PATH
    "ARM 版 libjpeg 的 install prefix。jpeg-arm-turbo=带 NEON 的 turbo，jpeg-arm=IJG 原版")

if(NOT EXISTS "${JPEG_ARM_DIR}/lib/libjpeg.a")
    message(FATAL_ERROR
        "找不到 ARM 版 libjpeg: ${JPEG_ARM_DIR}/lib/libjpeg.a\n"
        "  turbo 版跑: tools/build_libjpeg_turbo.sh\n"
        "  IJG  版跑: tools/build_libjpeg.sh")
endif()

target_include_directories(camera PUBLIC "${JPEG_ARM_DIR}/include")
target_link_libraries(camera PUBLIC "${JPEG_ARM_DIR}/lib/libjpeg.a")
