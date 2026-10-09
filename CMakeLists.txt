cmake_minimum_required(VERSION 3.13)
project(lvgl_camera C)

# 标准由 toolchain.cmake 统一指定（gcc 4.9.4 默认 gnu90，不设会编译不过）

# ---------------------------------------------------------------------------
# LVGL 构建裁剪
#
# 这些必须在 add_subdirectory(lvgl) 之前设好，否则 LVGL 的 option() 已经
# 把默认值写进 cache 了，再改就被忽略。
# ---------------------------------------------------------------------------

# demos/examples 有几百个 .c，我们一个都不用，编进去纯浪费几分钟
set(CONFIG_LV_BUILD_DEMOS    OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# ThorVG 是 C++ 矢量图形引擎，这个项目用不上
set(CONFIG_LV_USE_THORVG     OFF CACHE BOOL "" FORCE)

set(LV_BUILD_INSTALL         OFF CACHE BOOL "" FORCE)

# 交叉编译的三条保命线：
#   find_package / pkg-config 会去宿主机找库 -> 链进 x86 的 .so，板子跑不了
#   FetchContent 会联网下载依赖 -> 离线环境直接卡死
# 我们只用 fbdev + evdev，两者都在内核头文件里，不需要任何外部依赖。
set(LV_USE_FIND_PACKAGE   OFF CACHE BOOL "" FORCE)
set(LV_USE_PKG_CONFIG     OFF CACHE BOOL "" FORCE)
set(LV_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)

# lv_conf.h 放在本文件同级目录（项目根），LVGL 的 CMake 会自动认这个位置，
# 走 LV_CONF_INCLUDE_SIMPLE 分支，不需要额外传路径。
add_subdirectory(lvgl)

# ---------------------------------------------------------------------------
# 应用
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# 采集层：V4L2 采集 + 环形队列 + 解码
#
# 链接的是上游 ~/V4L2-Camera-App 交叉编译后 install 出来的产物，本仓库不放
# 它的源码——上游本来就把库编成 libcamera.a，那就是给人链接用的。两个前缀
# 都由 tools/ 下的脚本生成，见 README「构建 → 前置」。
# ---------------------------------------------------------------------------
set(LIBCAMERA_DIR "${CMAKE_SOURCE_DIR}/libcamera-arm"
    CACHE PATH "libcamera.a 的 install prefix")
set(JPEG_ARM_DIR  "${CMAKE_SOURCE_DIR}/jpeg-arm-turbo"
    CACHE PATH "ARM 版 libjpeg 的 install prefix")

if(NOT EXISTS "${JPEG_ARM_DIR}/lib/libjpeg.a")
    message(FATAL_ERROR
        "找不到 ARM 版 libjpeg-turbo: ${JPEG_ARM_DIR}/lib/libjpeg.a\n"
        "  先跑: tools/build_libjpeg_turbo.sh")
endif()
if(NOT EXISTS "${LIBCAMERA_DIR}/lib/libcamera.a")
    message(FATAL_ERROR
        "找不到 ARM 版 libcamera.a: ${LIBCAMERA_DIR}/lib/libcamera.a\n"
        "  先跑: tools/build_libcamera.sh")
endif()

# UI 全部用图标，不再需要中文字体——图标取自 LVGL 内置的
# lv_font_montserrat_24（LV_SYMBOL_* 就编在 Montserrat 的私用区字形里），
# 见 lv_conf.h。
add_executable(lvgl_camera
    app/main.c
    app/camera_preview.c
    app/photo_store.c
    app/photo_view.c
)

# 头文件来自 libcamera 的 install prefix（app/ 里都是 #include "camera/xxx.h"）
target_include_directories(lvgl_camera PRIVATE "${LIBCAMERA_DIR}/include")

# 两个 .a 都写全路径。静态库之间不传递依赖，libcamera.a 用的 libjpeg
# 得在这里显式链上，否则是一堆未定义符号。
# pthread：camera_preview 的解码线程，以及 libcamera 内部的采集线程
target_link_libraries(lvgl_camera PRIVATE
    lvgl::lvgl
    "${LIBCAMERA_DIR}/lib/libcamera.a"
    "${JPEG_ARM_DIR}/lib/libjpeg.a"
    m pthread
)

# 只给自己写的代码开告警，不加到 lvgl 上——那会淹掉我们自己真正的问题。
# 之前没开，未使用的变量一声不响地留着，是个教训。
target_compile_options(lvgl_camera PRIVATE -Wall -Wextra)
