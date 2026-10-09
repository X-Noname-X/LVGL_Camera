# 采集层：V4L2 采集 + 有界环形队列 + 解码，编成 libcamera.a
#
# 来源：从 ~/V4L2-Camera-App 拷贝而来（capture / decoder / frame_queue），
# 两边逐字节一致，上游在那里。
#
# 要改就改上游，再整体拷回来；直接改这里会让两边悄悄分叉，
# 下次同步时谁覆盖谁说不清。它原项目的 CMakeLists 还依赖 SDL2 和
# 宿主机的 libjpeg（那是给 PC 预览窗口用的），这里不用那份，只取库的部分。

add_library(camera STATIC
    frame_queue.c
    decoder.c
    capture.c
)

target_include_directories(camera PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(Threads REQUIRED)
target_link_libraries(camera PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# libjpeg：必须用 libjpeg-turbo 的 ARM 版，由 tools/build_libjpeg_turbo.sh
# 交叉编译到 jpeg-arm-turbo/。
#
# 不能用 find_package(JPEG)——那会找到宿主机的
# /usr/lib/x86_64-linux-gnu/libjpeg.a，链进去要么报架构不匹配，
# 要么侥幸通过后在板子上跑不起来。
#
# 也不能退回 IJG 原版：decoder.c 靠 JCS_RGB565 直出 RGB565，
# 那是 turbo 的扩展，IJG 版编不过；而且 IJG jpeg-9b 在 792MHz 的
# Cortex-A7 上解 640x360 要 67ms，只能到 14fps。
# ---------------------------------------------------------------------------
set(JPEG_ARM_DIR "${CMAKE_SOURCE_DIR}/jpeg-arm-turbo")

if(NOT EXISTS "${JPEG_ARM_DIR}/lib/libjpeg.a")
    message(FATAL_ERROR
        "找不到 ARM 版 libjpeg-turbo: ${JPEG_ARM_DIR}/lib/libjpeg.a\n"
        "  先跑: tools/build_libjpeg_turbo.sh")
endif()

target_include_directories(camera PUBLIC "${JPEG_ARM_DIR}/include")
target_link_libraries(camera PUBLIC "${JPEG_ARM_DIR}/lib/libjpeg.a")
