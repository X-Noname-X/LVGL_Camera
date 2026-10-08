# LVGL_Camera

正点原子 IMX6ULL 上的嵌入式相机：USB 摄像头实时预览 + 拍照 / 相册 / 删除，
UI 用 LVGL v10 搭建，全屏输出到 `/dev/fb0`。

当前预览性能 **28.7 fps @ 640×360**（摄像头上限 30 fps 的 95.7%），
从最初的 13.9 fps 优化上来。完整过程见文末[性能优化记录](#性能优化记录)。

---

## 硬件与环境

| | |
|---|---|
| 开发板 | 正点原子 (ALIENTEK) IMX6ULL，Cortex-A7 @ **792 MHz**（满频，未降频） |
| 屏幕 | **480×272 RGB，16bpp (RGB565)**，`/dev/fb0` |
| 触摸 | evdev，`/dev/input/event1` |
| 摄像头 | USB UVC 免驱，**`/dev/video2`**，MJPEG，支持 30 fps |
| 内核 | Linux 4.1.15（`~/linux`，`imx6ull-alientek-emmc.dts`） |
| 交叉工具链 | Linaro GCC **4.9.4** (2017.01)，glibc 2.19 |
| LVGL | v10.0 |

> **摄像头是 `/dev/video2` 不是 `/dev/video0`**。UVC 摄像头常导出多个 video 节点，
> video0/video1 可能是 metadata 节点，只有 video2 真正出图像。
> 用 `v4l2-ctl --list-formats-ext -d /dev/videoN` 确认哪个有采集格式。

---

## 当前进度

**功能已全部完成。**

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | LVGL 点亮：`/dev/fb0` 显示 + 触摸 | ✅ |
| 2 | 摄像头实时预览 | ✅ 28.7 fps |
| 3 | 按键拍照保存 | ✅ 零编码，存原始 MJPEG 帧 |
| 4 | 相册浏览 | ✅ 单张大图 + 左右翻页 |
| 5 | 删除图片 | ✅ 带二次确认 |

### 界面语言约定

| 位置 | 用什么 | 为什么 |
|---|---|---|
| **按键** | 图标（`LV_SYMBOL_*`） | 图标字体白拿，不用自造 |
| **屏幕上的提示文字** | 英文 / ASCII | 项目不含中文字体，写汉字会是方框 |
| **终端 printf 日志** | 中文 | 终端能正常显示 UTF-8，中文更省事 |

图标不是独立字体——`LV_SYMBOL_*` 是一组 Unicode 私用区码点，字形直接编在
Montserrat 里。所以 `lv_conf.h` 开一个 `LV_FONT_MONTSERRAT_24` 就白得 60 个图标。

---

## 目录结构

```
LVGL_Camera/
├── app/
│   ├── main.c                  UI 搭建、LVGL 驱动注册、主循环
│   ├── camera_preview.c/.h     采集线程管理、缩放、三缓冲发布
│   ├── photo_store.c/.h        照片文件：命名、列目录、删除
│   └── photo_view.c/.h         读文件 → 解码 → 缩放适配
├── libcamera/                  V4L2 采集 + 环形队列 + 解码（见下方"来源"）
│   ├── capture.c               设备打开、格式协商、mmap、采集线程
│   ├── frame_queue.c           有界环形缓冲，满了丢旧留新
│   ├── decoder.c               MJPEG / YUYV → RGB565 或 RGB24
│   └── include/camera/         三个头文件
├── lvgl/                       LVGL v10.0 源码（上游原样）
├── tools/
│   ├── build_libjpeg_turbo.sh  交叉编译 libjpeg-turbo + NEON
│   ├── build_libjpeg.sh        交叉编译 IJG jpeg-9b（回退用）
│   └── gen_font.sh             生成中文字体子集（现已不用，留作备查）
├── lv_conf.h                   LVGL 配置，文件头列了全部改动
├── toolchain.cmake             锁定工具链与 sysroot
└── CMakeLists.txt
```

**`libcamera/` 的来源**：从 `~/V4L2-Camera-App` 拷贝而来（那是个 PC 上的
多线程 V4L2 采集库，用 SDL2 开预览窗口）。本项目只取它的库部分，
并做了几处扩展，都在 `decoder.h` 里标了「上游没有」：
`decoder_set_mjpeg_rgb565()`、`decoder_decode_jpeg()`、`decoder_jpeg_size()`、
`decoder_have_rgb565()`。**改动请同步两边。**

---

## 构建

### 前置：两个一次性步骤

下面这些源码包**不在 git 仓库里**（`.gitignore` 忽略了 `*.tar.gz`），
需要自己准备，放到项目根目录：

| 文件 | 用途 | 来源 |
|---|---|---|
| `gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf.tar.xz` | 交叉工具链 | 正点原子 SDK |
| `libjpeg-turbo-3.0.4.tar.gz` | MJPEG 解码（默认） | GitHub: `libjpeg-turbo/libjpeg-turbo` releases |
| `jpegsrc.v9b.tar.gz` | MJPEG 解码（IJG 回退，可选） | `~/embedded-linux-simple-camera/thirdlibs/` |

**① 解压工具链**（约 430 MB，解到项目根目录）

```bash
tar -xJf gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf.tar.xz
```

> 大写的 `-J` 是 xz 格式；小写 `-j` 是 bzip2，会报错。
> 工具链位置在 `toolchain.cmake` 里，换位置用 `-DTC_ROOT=/新路径` 覆盖，不用改文件。

**② 交叉编译 libjpeg-turbo（带 NEON）**

```bash
tools/build_libjpeg_turbo.sh
```

产物在 `jpeg-arm-turbo/`（约 2.2 MB）。
这一步是性能的关键——见[性能优化记录](#性能优化记录)第 ① 项。

### 编译

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake -B build
cmake --build build -j$(nproc)
```

产物 `build/lvgl_camera`（约 1.2 MB）。
我们自己的代码开了 `-Wall -Wextra`；不加到 lvgl 上，否则会淹掉真正的问题。

### 回退到 IJG 原版 libjpeg 对比

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake -B build \
      -DJPEG_ARM_DIR=$PWD/jpeg-arm
cmake --build build -j$(nproc)
```

换回 turbo 就去掉 `-DJPEG_ARM_DIR` 重新 configure。
`decoder.c` 里用 `#ifdef LIBJPEG_TURBO_VERSION` 做了保护，
IJG 版拿不到 RGB565 路径会自动退回 RGB24（**别改成 `#ifdef JCS_RGB565`**，
原因见文末踩坑第 1 条）。

---

## 运行

```bash
./lvgl_camera
```

四个参数都有默认值，直接跑即可：

| 参数 | 默认值 |
|---|---|
| fbdev | `/dev/fb0` |
| input_event | `/dev/input/event1` |
| video_dev | `/dev/video2` |
| photo_dir | `/run/media/mmcblk0p1/photos` |

需要覆盖时才按位置传参，比如触摸没反应要换个 event 号：

```bash
./lvgl_camera /dev/fb0 /dev/input/event0
```

> **照片目录为什么是 `/run/media/mmcblk0p1`**：那是系统自动挂载 SD 卡的位置，
> 插卡就挂、重启后依然是这个路径。而自己 `mkdir` + `mount` 的 `/mnt/sdcard`
> 有个静默陷阱——卡没挂时它**依然"存在"**（是根文件系统上的空目录），
> 照片会被悄悄写进 eMMC 且不报任何错。已经踩过一次。
> 用 `/run/media/...` 的话卡不在就压根没这个路径，启动即报错，失败得早。

`Ctrl+C` 的收尾顺序：

1. 停采集与解码线程（不会把摄像头设备留在半开状态）
2. 删掉预览定时器
3. **把屏幕清成黑色**

第 3 步是必须的——不清的话 LCD 会一直停着最后一帧预览画面，
看上去像程序还在跑、或者像死机了。

启动后每秒输出一行统计：

```
[预览] 28.7 fps | 解码+转换 34.8 ms/帧 | 队列丢帧 13
```

- **fps** — 实际完成「解码 + 转换 + 发布」的帧率
- **ms/帧** — 单帧「解码 + 转换」平均耗时（不含队列等待）
- **队列丢帧** — **累计值**，只增不减。要看的是它的**增速**：

  ```
  丢帧速率 ≈ 摄像头帧率(30) - fps
  ```

  两者对不上说明计时有问题。丢帧由 `frame_queue.c` 统计，
  和 fps 计时器是两套独立代码，可以互证。

---

## 架构

### 线程模型（3 个线程）

```
libcamera 采集线程          DQBUF → fq_push（阻塞式，靠 STREAMOFF 唤醒退出）
        ↓ 环形队列（3 槽，FQ_DROP_OLDEST）
本模块解码线程              fq_pop → libjpeg 解码 → 缩放 → 发布
        ↓ 三缓冲（互斥锁保护索引）
LVGL 主线程                 lv_timer(33ms) → 取裸指针 → 渲染
```

> ⚠️ **LVGL 不是线程安全的。解码线程绝不调用任何 `lv_*` 函数。**
> 两边只通过 `cam_preview_frame()` 交换一个裸缓冲指针，
> 由主线程去 `lv_image_set_src` + `lv_obj_invalidate`。
> 违反这条会随机崩溃，且极难定位。

### 数据流与各环节的数据量

```
MJPEG 压缩帧            ~20 KB      ← USB 送来
      ↓ libjpeg-turbo + NEON 解码（含反 DCT、色度上采样、YUV→RGB）
RGB565 解码缓冲         450 KB      ← 640×360×2
      ↓ 等比缩放 + 居中（最近邻，查表）
RGB565 显示缓冲         255 KB      ← 480×270×2，三份共 765 KB
      ↓ LVGL 贴图（零转换）
fb0 帧缓冲              255 KB      ← 16bpp，与上面一一对应
```

**整条链路只有一次像素格式变动**，就是解码那一步。
解码输出、显示缓冲、fb0 三者都是 RGB565，所以从解码之后到屏幕没有任何格式转换。

### 三缓冲为什么是 3 不是 2

发布第 N 帧后，生产端要写满另外**两块**才回到同一块，给了消费端整整 2 个帧周期。
双缓冲在 LVGL 渲染偶尔卡一下时会撕裂，多这 255 KB 换掉那个隐患很划算。

---

## 关键配置

### `lv_conf.h`

文件头列了相对模板的全部改动。几个容易踩的：

| 选项 | 值 | 原因 |
|---|---|---|
| `LV_USE_STDLIB_MALLOC` 等三项 | `LV_STDLIB_CLIB` | **模板默认是 `LV_STDLIB_BUILTIN`，内存池只有 64 KB**，给单片机用的。Linux 上必须换 glibc malloc |
| `LV_COLOR_FORMAT_DEFAULT` | `RGB565` | **`LV_COLOR_DEPTH` 在本版本已废弃**，别再用它 |
| `LV_LOG_PRINTF` | `1` | **不开的话所有 `LV_LOG_*` 被静默丢弃**，驱动内部的诊断信息全看不到 |
| `LV_USE_LINUX_FBDEV` / `LV_USE_EVDEV` | `1` | v10 自带这两个驱动，**不需要写移植层** |
| `LV_FONT_SOURCE_HAN_SANS_SC_*_CJK` | `0` | 残缺子集，用不上；改用自生成的字体 |

### `toolchain.cmake`

关键在 `CMAKE_FIND_ROOT_PATH_MODE_*` 那几行——**禁止到宿主机找头文件和库**。
不设的话 CMake 很可能把宿主机的 x86 `libjpeg.a` / `libpng` 链进来，
结果就是板子上跑不起来或符号找不到。

---

## 常见问题

**触摸没反应** — 多半是 event 号不对，见 `/proc/bus/input/devices` 里
touchscreen 那段的 `Handlers=`。不用重新编译，换第二个参数即可。

**屏幕上一片方块** — 往界面上写了汉字。项目里**没有中文字体**，
只剩 Montserrat 一套拉丁字形。屏幕上的文案要么用 `LV_SYMBOL_*` 图标，
要么用 ASCII 英文；终端 `printf` 不受此限，照旧用中文。

**照片存到了 eMMC 上而不是 SD 卡** — 检查照片目录。默认是
`/run/media/mmcblk0p1/photos`，卡不在时这个路径不存在、启动即报错，
不会静默写错地方。反过来，如果用的是自己 `mkdir` + `mount` 的路径
（比如 `/mnt/sdcard`），卡没挂时程序察觉不到，照片会悄悄落进 eMMC。

**"打开 /dev/fb0 失败"却没有任何 errno 输出** — 典型的 LVGL 内存池耗尽。
该失败路径只写 `LV_LOG_ERROR` 不 `perror`，所以必须先打开 `LV_LOG_PRINTF`
才能看到真正原因。见 `lv_conf.h` 的 STDLIB 配置。

**帧率明显低于 30** — 先看日志里的 `ms/帧`。若接近 33 ms，说明解码是瓶颈，
参考[性能优化记录](#性能优化记录)的「还没做的优化」。

---

## 性能优化记录

> 以下是阶段 2 完成后，把预览从 13.9 fps 做到 28.7 fps 的完整过程。
> 所有数字都是**板子上实测**的，不是估算。

### 结果

| | 优化前 | 优化后 |
|---|---|---|
| 摄像头分辨率 | 640×360 | 640×360（未变） |
| 预览面积 | 480×270 | 480×270（未变） |
| 单帧耗时 | **71.6 ms** | **34.8 ms** |
| 帧率 | **13.9 fps** | **28.7 fps** |
| 丢帧速率 | 16.6 /秒 | **1.5 /秒** |

耗时降到 49%，帧率翻倍，丢帧减少 91%——**且画面尺寸一点没缩水**。

### ① 换用 libjpeg-turbo + NEON　71.6 → 38.1 ms（1.88×）

**问题定位**：71.6 ms 里解码占约 67 ms，转换只占约 5 ms（按输出像素数估算），
所以优化转换循环没有意义，必须降解码成本。

**原因**：原本链的是 IJG 原版 jpeg-9b，**纯 C、零 SIMD**。实测
230,400 像素 / 67 ms ≈ 3.5 Mpixel/s，这正是 IJG libjpeg 在 ARM 上的典型水平。
而 Cortex-A7 有 NEON（`/proc/cpuinfo` 的 `Features` 里有 `neon`），
libjpeg-turbo 的 NEON 路径能显著加速。

**做法**：`tools/build_libjpeg_turbo.sh`，用同一套 Linaro 工具链交叉编译
libjpeg-turbo 3.0.4，开 `-mcpu=cortex-a7 -mfpu=neon`。

**验证**（不能只看编译日志说 "SIMD enabled" 就信）：

- `HAVE_NEON - Success`、`SIMD extensions: arm`
- `jsimd_neon.S.o` 里反汇编出 **479 条 NEON 指令**
- 最终 `lvgl_camera` 二进制里 **6882 条 NEON 指令**
- 体积反而从 1,427,893 降到 1,219,049 字节

**注意**：GCC 4.9.4 太老，NEON 内建函数不全，CMake 自动选了 **GAS 汇编实现**
（`NEON_INTRINSICS=OFF`）——按上游注释，这反而是性能更好的那个。

### ② JCS_RGB565 直接输出　38.1 → 34.8 ms（再快 8.7%）

**做法**：libjpeg-turbo 支持 `JCS_RGB565` 扩展，让解码直接产出 2 字节/像素，
省掉调用方一整遍 RGB24→RGB565 的逐像素转换。

**三个从源码里挖出来、决定成败的细节**（文档里没有，只能读 `jdcolor.c`
和 `jdmaster.c`）：

1. **字节序不用处理**。`PACK_SHORT_565_LE(r,g,b)` 把 R 放在 bit15-11，
   和 LVGL 在小端机上的 RGB565 布局**逐位一致**。
2. **必须关掉 `do_fancy_upsampling`**。`jdmaster.c` 的 `use_merged_upsample()`
   里有 `if (cinfo->do_fancy_upsampling || ...) return FALSE;`，而这个选项
   **默认是 TRUE**，会把「色度上采样 + 色彩转换」的合并快路径禁掉。
3. `JCS_RGB565` 的 `out_color_components` 是 **3 而不是 2**（`jdmaster.c:352`），
   这是通过合并路径检查的必要条件。

**副作用**：中间缓冲从 691 KB 降到 450 KB，`convert_scale` 的 565 分支
变成纯 16 位搬运、零格式转换。

### ③ 分辨率选 640×360 而非 320×240（不是性能优化，是让性能花在刀刃上）

摄像头支持 8 种分辨率。选 640×360 (16:9 = 1.778) 是因为屏是 480×272 (1.765)，
**两者比例几乎一致**——缩放到铺满整屏只需上下各留 1px，**视野零损失**。

用 4:3 的话，满高下宽度上限只有 `272 × 4/3 ≈ 363 px`，右侧必然空一条；
而且这个 363 是**硬上限，把按键缩到多小都补不回来**。

代价是解码像素数变成 3 倍，所以 ① 和 ② 是它的前提。

### 还没做、但可以做的优化

按性价比排序。**当前 28.7 fps 对摄像头 30 fps 已是 95.7%，下面都不是必须的。**

**A. `JDCT_IFAST` 反 DCT　预计 10~20%，改动 1 行**

```c
cinfo.dct_method = JDCT_IFAST;
```

libjpeg 默认用 `JDCT_ISLOW`（精确整数反 DCT）。换成快速版本后**代价几乎免费**——
反 DCT 的精度只影响**预览画面**，而拍照存的是摄像头原始 MJPEG 帧，
压根不经过解码器，所以照片画质完全不受影响。

预计能把 34.8 ms 压到 30 ms 左右，坐稳 30 fps。
（上游的 CMake 测试用例里有 `rgb565 + ifast` 组合，是受支持的搭配。）

**B. 队列内存 1350 KB → 约 90 KB　省 1.26 MB**

```
[预览] 内存: 输出 765KB, 解码缓冲 450KB, 队列 1350KB
                                          ^^^^^^^^^^
```

1350 KB = 3 槽 × 460,800 字节，而 460,800 = 640×360×**2**，
是**驱动给 MJPEG 按未压缩尺寸预留的上界**（`capture.h` 有说明）。
MJPEG 实际每帧只有 20 KB 左右，**约 99% 是浪费的**。

要省这块得绕开 `cap_start` 的校验（它强制要求槽容量 ≥ `cap_max_frame_size`）。
1.35 MB 对 256 MB 内存无所谓，不急。

**C. `convert_scale` 去掉全屏背景填充　省约 0.2~0.5 ms**

现在每帧先把整屏 480×272 刷成背景色，然后**覆盖掉其中 99.26%**。
本例 `disp_w == dst_w`（480 == 480），左右根本没有留边，
只有上下各 1 px 需要背景。改成只填这两行即可。

**D. `convert_scale` 双线性缩放（画质，不是性能）**

现在用最近邻。**640→480 是缩小**，最近邻缩小会丢像素、边缘可能有锯齿
（放大没这个问题）。换双线性约多 3~4 ms，会吃掉 A 的收益。

**E. `NEON_INTRINSICS=ON` 重编对比**

当前走 GAS 汇编路径。可试 `-DNEON_INTRINSICS=ON` 走内建函数路径，
在 GCC 4.9.4 上哪个快没有定论，实测为准。

**F. 不建议：848×480**

比例和屏几乎完全一致（1.7667 vs 1.7647），能做到零裁剪，
但解码量是 640×360 的 1.77 倍，A7 上大概率得不偿失。
1280×720 更是完全不用考虑。

### 优化过程中踩的坑

**1. `#ifdef JCS_RGB565` 恒为假。**
`JCS_RGB565` 是 `J_COLOR_SPACE` 枚举里的**枚举常量**，不是宏，
而 `#ifdef` 只检测宏。结果是明明链着 turbo 却一直走 RGB24 路径，
**且编译期毫无提示**。正确做法是检测 `LIBJPEG_TURBO_VERSION`
（turbo 的 `jconfig.h` 用 `#define` 给出，IJG 版没有）。

**2. "零告警"曾经不可靠。**
`CMakeLists.txt` 一度没加 `-Wall`，留下 3 个未使用变量却一声不响。
现已对**我们自己写的代码**开 `-Wall -Wextra`。

**3. LVGL 内置的 CJK 字体是残缺子集。**
那两款 `lv_font_source_han_sans_sc_*_cjk` 并不是完整思源黑体，实测只覆盖
1450 个码点、其中汉字仅 **297** 个（`range_length = 52772` 只是 cmap 声明的
跨度，真实字符数看 `list_length`）。当时 UI 用字一个都不在里面，满屏方块。

> 这是**历史**：界面后来改成「图标 + 英文」，中文字体已经整个移除了，
> 现在用内置的 `LV_FONT_MONTSERRAT_24`。`tools/gen_font.sh` 保留备查。

**4. `lv_font_conv` 合并多字体时会静默丢字形。**
某个字符若被后声明的字体请求、而那个字体没有，它会把先声明字体
**已经提供**的同一个字形一并抹掉。所以分工必须干净：汉字和中文标点走 CJK 源，
ASCII 和排印标点走拉丁源，不重叠。`tools/gen_font.sh` 末尾带自动校验。
（同上，这是历史经验，脚本现已不用。）

**5. LVGL 的 STDLIB 默认值是给单片机的。**
`LV_USE_STDLIB_MALLOC` 默认 `LV_STDLIB_BUILTIN`，内存池只有 64 KB。
该失败路径只写 `LV_LOG_ERROR` 不 `perror`，症状是「打开 /dev/fb0 失败」
却没有任何 errno 输出，极难定位。

**6. 忘了 LVGL 按创建顺序叠放。**
浮动提示 `g_toast` 一度建在相册页**之前**，被相册页整个盖住——
相册里删完照片那句「Deleted」根本看不见。凡是"要盖在所有东西之上"的
浮层，创建顺序必须排在最后。

**7. 缺头文件靠传递包含侥幸编译。**
`main.c` 用 `memset` 却没有 `<string.h>`、`photo_store.c` 用 `malloc`/`qsort`
却没有 `<stdlib.h>`，都是靠别的头文件顺带包含进来的。**两次都栽在同一件事上。**
换了包含顺序或编译器版本就会突然编不过。

**8. 测试用例抓到的行为问题：小照片被放大。**
`photo_view` 原本会把比显示区小的照片放大填满，既费 CPU 又糊。
改成**不放大**——照片小于显示区就按原尺寸出。

**9. LVGL 默认主题给按钮加了向下的投影。**
`src/themes/default/lv_theme_default.c` 的 `styles.btn` 里有：

```c
lv_style_set_shadow_color(&theme->styles.btn, lv_palette_main(LV_PALETTE_GREY));
lv_style_set_shadow_width(&theme->styles.btn, LV_DPX_CALC(dpi, 3));
lv_style_set_shadow_opa(&theme->styles.btn,   LV_OPA_50);
lv_style_set_shadow_offset_y(&theme->styles.btn, LV_DPX_CALC(dpi, 3));  /* 向下偏 3 */
```

按钮浮在预览画面和照片上时，这圈灰影就表现为**下半边发虚、像重影**。
主题里 `radius`/`bg_opa`/`bg_color`/`pad` 都覆盖了，唯独容易漏掉阴影——
`lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN)` 清掉即可。

**10. 忘了 LVGL 按创建顺序叠放（第二次踩）。**
见上文第 6 条。同一个坑在这个项目里踩了两次，新增浮层时先想清楚它该在谁上面。

**11. 退出时屏幕停在最后一帧。**
`Ctrl+C` 后程序退了，但 framebuffer 里还是最后那帧预览，看上去像还在跑或者死机。
收尾时清一次屏即可：删掉预览定时器（否则它会往已释放的对象上刷）→
`lv_obj_clean()` + 底色改黑 + `lv_refr_now()` 强制立刻重绘一次。
