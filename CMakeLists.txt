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
# 采集层：V4L2 采集 + 环形队列 + 解码（libcamera.a）
# 它自己会去找 ARM 版 libjpeg，见 libcamera/CMakeLists.txt
# ---------------------------------------------------------------------------
add_subdirectory(libcamera)

# font_zh_16.c 是 tools/gen_font.sh 生成的中文字体子集，不是手写的，
# 但必须参与编译。改了 UI 文案要用字后重跑那个脚本再重新构建。
add_executable(lvgl_camera
    app/main.c
    app/camera_preview.c
    app/photo_store.c
    app/font_zh_16.c
)

# pthread：camera_preview 的解码线程，以及 libcamera 内部的采集线程
target_link_libraries(lvgl_camera PRIVATE lvgl::lvgl camera m pthread)

# 只给自己写的代码开告警，不加到 lvgl 上——那会淹掉我们自己真正的问题。
# 之前没开，未使用的变量一声不响地留着，是个教训。
target_compile_options(lvgl_camera PRIVATE -Wall -Wextra)
