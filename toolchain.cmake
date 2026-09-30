# 交叉编译工具链：Linaro GCC 4.9.4 (2017.01)，ARM 32位 hard-float，glibc 2.19
#
# 用法：cmake -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake -B build
#
# TC_ROOT 是 CACHE 变量，工具链换位置了用 -DTC_ROOT=/新路径 覆盖即可，
# 不用改这个文件。

set(TC_ROOT "/home/xyq/LVGL_Camera/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf"
    CACHE PATH "交叉编译工具链根目录")

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER   "${TC_ROOT}/bin/arm-linux-gnueabihf-gcc")
set(CMAKE_CXX_COMPILER "${TC_ROOT}/bin/arm-linux-gnueabihf-g++")

# 这个工具链自带独立 sysroot，gcc 能自己找到；
# 显式写出来是为了让 CMake 的 find_* 也只在它里面找。
set(CMAKE_SYSROOT "${TC_ROOT}/arm-linux-gnueabihf/libc")

# 关键：禁止到宿主机（x86 Ubuntu）去找头文件和库。
# 不设这几行，CMake 很可能把宿主机的 libjpeg/libpng 链进来，
# 结果就是板子上跑不起来或符号找不到。
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# gcc 4.9.4 默认是 gnu90，LVGL v10 需要 C99 以上。
# 这里不设，编译 LVGL 时会冒出一堆 "for 循环内声明变量" 之类的报错。
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
