/*
 * 阶段 4：实时预览 + 拍照 + 相册
 *
 * 已通：LVGL 画到 /dev/fb0、触摸从 /dev/input/eventX 进来、摄像头实时画面、
 *       按「拍照」存 JPEG、按「相册」单张大图浏览并翻页。
 * 删除留到阶段 5。
 *
 * 拍照走的是零编码路径：摄像头输出 MJPEG，而每个 MJPEG 帧本身就是一张完整
 * JPEG 文件，所以直接写盘即可——没有编码开销，也没有画质损失，
 * 存下来还是摄像头原始分辨率(640x360)，不受屏幕尺寸影响。
 *
 * 两个页面用「相册整页盖住相机页」来切换，不是 LVGL 的多 screen——
 * 这样相机页的控件一个都不用重新挂父对象。代价是预览定时器要跳过刷新。
 *
 * 布局全部按显示驱动报告的实际分辨率算，不写死尺寸——
 * 换一块屏不用改代码，也不会出现按钮跑到屏幕外面的情况。
 *
 * 线程模型（见 camera_preview.h 的说明）：
 *   libcamera 采集线程 → 环形队列 → 本进程解码线程 → 三缓冲 → LVGL 主线程
 * 解码线程绝不碰 lv_*，两边只交换一个裸缓冲指针。
 *
 * 用法：./lvgl_camera [fbdev] [input_event] [video_dev] [photo_dir]
 *
 * 四个参数都有默认值，直接 ./lvgl_camera 即可：
 *   /dev/fb0  /dev/input/event1  /dev/video2  /run/media/mmcblk0p1/photos
 *
 * 需要覆盖时才传参，比如触摸没反应换个 event 号（见 /proc/bus/input/devices），
 * 或者把照片存到别的目录。
 */
#include "lvgl.h"
#include "camera_preview.h"
#include "photo_store.h"
#include "photo_view.h"

#include <limits.h>   /* PATH_MAX */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>   /* memset——之前一直靠其他头文件传递包含，很脆弱 */
#include <unistd.h>

#define DEF_FBDEV  "/dev/fb0"
#define DEF_INPUT  "/dev/input/event1"
#define DEF_V4L2   "/dev/video2"   /* 本板的 UVC 摄像头在 video2，不是 video0 */

/*
 * 默认照片目录。
 *
 * 选 /run/media/mmcblk0p1 而不是 /mnt/sdcard，是因为后者有个静默陷阱：
 * 自己 mkdir + mount 的路径在「已挂载」和「未挂载」两种状态下都"存在"，
 * 程序分辨不出来。卡没挂时照片会被悄悄写进 eMMC，不报任何错。
 * 已经踩过：重启后手动挂载失效，照片就跑到了根文件系统上。
 *
 * 而 /run/media/mmcblk0p1 由系统在插卡时自动挂载，卡不在时这个路径
 * 压根不存在——启动就会报「无法创建目录」。失败得早，比失败得安静好。
 */
#define DEF_PHOTO_DIR "/run/media/mmcblk0p1/photos"

/*
 * 期望的摄像头分辨率。
 *
 * 选 640x360 (16:9 = 1.778) 而不是 320x240 (4:3 = 1.333)，因为屏是 480x272
 * (1.765)，两者比例几乎一致——缩放到铺满整屏只需裁掉 0.74% 的视野。
 *
 * 用 4:3 的话，满高下宽度上限只有 272*4/3 ≈ 363px，右侧必然空掉一条，
 * 而把按键缩到最小也补不回来（363 是硬上限，再宽也没用）。
 *
 * 代价：解码像素数 76,800 → 230,400，3 倍。帧率要实测，见日志里的 fps 行。
 * 驱动可能给别的分辨率，代码按实际协商值等比适配。
 */
#define WANT_CAM_W 640
#define WANT_CAM_H 360

/* 浮动圆钮。屏只有 480x272，48px 的圆够手指点，再大就明显遮挡画面了 */
#define BTN_D      48
#define BTN_MARGIN 12
#define BTN_GAP    16

/* 预览刷新周期，约 30fps。真正的帧率由采集端决定，这里只是取帧频率 */
#define PREVIEW_PERIOD_MS 33

/*
 * 相册一次最多列多少张。用固定数组而不是动态分配：一张照片的文件名才
 * 12+ 字节，128 张也就 4KB，省去一套内存管理。
 * 超出部分会被静默丢弃——要改就改这里，photo_store_list 的注释里有说明。
 */
#define PHOTO_MAX_LIST 128

/*
 * 界面字体，就是放大一号的 Montserrat_24。
 *
 * 两个用途：
 *   1. 按钮图标——LV_SYMBOL_* 是一组 Unicode 私用区码点，字形直接编在
 *      Montserrat 里。所以开个够大的尺寸就能白拿几十个图标，
 *      不用自己做图标字体。48px 的圆钮配 24px 图标正好。
 *   2. 屏幕上那几句英文提示（"No photos" 之类）——字体本身是拉丁字体，
 *      渲染 ASCII 是本职工作。用 24px 而不是默认的 14px，是因为
 *      屏幕只有 480x272，小字看着费劲。
 *
 * 图标用法就是把 LV_SYMBOL_xxx 当普通字符串传给 lv_label_set_text()，
 * 渲染时自然落到这个字体的私用区字形上。
 *
 * 注意屏幕上的文字只能用 ASCII——中文字体已经删了，写汉字会是方框。
 * 终端的 printf 日志不受此限，照旧用中文。
 */
#define ICON_FONT (&lv_font_montserrat_24)

/*
 * Ctrl+C 退出标志。有采集和解码线程在跑，不能直接被杀——那样摄像头设备
 * 不会被正常关闭、mmap 也不会解绑。走这个标志能让主循环退出后按顺序收摊。
 * sig_atomic_t 保证在信号处理函数里读写是安全的。
 */
static volatile sig_atomic_t g_quit = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

/*
 * 预览用的图像描述符。内容每帧都在变，所以只改 .data 指向的那块缓冲、
 * 再 invalidate，不重建控件——重建的话每帧都要走一遍对象分配和布局。
 *
 * header 的字段（w/h/stride/cf/magic）在启动时按预览区尺寸填一次即可。
 */
static lv_image_dsc_t g_img_dsc;
static lv_obj_t      *g_preview_img;
static int            g_src_set;
static lv_timer_t    *g_preview_timer;   /* 退出前要停掉它再清屏 */

/* -------------------------------------------------------------------------
 * 相册页
 *
 * 页面切换用「全屏不透明容器盖住」而不是 LVGL 的多 screen：相册页盖满整屏，
 * 显示时把相机页整个遮住即可，不用把相机页的控件重新挂到别的父对象上。
 * 代价是要在预览定时器里跳过刷新（见 preview_timer_cb），否则会在看不见的
 * 地方白刷一帧。
 * ------------------------------------------------------------------------- */
static lv_obj_t      *g_page_album;      /* 全屏容器，默认隐藏 */
static lv_obj_t      *g_album_img;       /* 照片 */
static lv_obj_t      *g_album_label;     /* 「第 N / 共 M 张」 */
static lv_image_dsc_t g_album_dsc;       /* 指向 photo_view 的内部缓冲 */
static lv_obj_t      *g_album_hint;      /* 空相册/打不开时的提示文字 */

static char g_names[PHOTO_MAX_LIST][PHOTO_NAME_MAX];
static int  g_count;                     /* 当前列表里有多少张 */
static int  g_index;                     /* 正在看第几张（0 起） */
static int  g_photo_area_w, g_photo_area_h;   /* 照片可用区尺寸 */

/* -------------------------------------------------------------------------
 * 浮层提示：显示一小会儿自动隐藏。
 * 拍照是个瞬时动作，需要一点反馈告诉用户「按到了、存好了」。
 * ------------------------------------------------------------------------- */
static lv_obj_t *g_toast;

static void toast_hide_cb(lv_timer_t *t)
{
    lv_obj_set_hidden(g_toast, true);
    lv_timer_delete(t);
}

static void toast_show(const char *text, uint32_t ms)
{
    lv_label_set_text(g_toast, text);
    lv_obj_set_hidden(g_toast, false);
    /* 重复次数设 1 = 一次性，跑完自己删掉，不用手工管理定时器 */
    lv_timer_set_repeat_count(lv_timer_create(toast_hide_cb, ms, NULL), 1);
}

/*
 * 定时取帧。这是 LVGL 主线程，也是唯一允许碰 lv_* 的地方。
 *
 * 取到的指针指向 camera_preview 内部的三缓冲之一：那边发布新帧后会先写满
 * 另外两块才回到这一块，所以这里渲染期间它不会被覆盖。
 *
 * 每帧只改 .data 再 invalidate，不重建控件、也不重复 set_src：
 * LVGL 对未压缩格式是直接使用这个指针（lv_bin_decoder.c 的 use_directly 分支），
 * 不会拷贝；且本项目 LV_CACHE_DEF_SIZE=0，图像缓存关着，每次绘制都重读 .data。
 */
static void preview_timer_cb(lv_timer_t *t)
{
    (void)t;

    /*
     * 先处理拍照落盘。
     *
     * 写 20KB 到 flash 要 10~50ms，会让这一轮渲染晚一点——但用户刚按下快门，
     * 这点停顿符合预期，也省掉了专门开一个写盘线程。放在取帧之前，
     * 这样即使此刻恰好在队列超时期间、没有新预览帧，也能及时把照片存下来。
     */
    const uint8_t *jpeg = NULL;
    size_t n = cam_preview_take_captured(&jpeg);
    if (n > 0) {
        char name[64];
        if (photo_store_save(jpeg, n, name, sizeof(name)) == 0) {
            char msg[96];
            /* 终端日志照旧用中文；屏幕上是英文——中文字体已经删掉，
             * 界面上只剩 Montserrat 一套拉丁字形，写汉字会是方框 */
            printf("[拍照] 已保存 %s/%s（%zu 字节，共 %d 张）\n",
                   photo_store_dir(), name, n, photo_store_count());
            fflush(stdout);
            snprintf(msg, sizeof(msg), "Saved %s", name);
            toast_show(msg, 1500);
        }
        else {
            toast_show("Save failed", 2000);
        }
    }

    /*
     * 相册页显示时它整个盖住了预览，刷新看不见的画面纯属白烧 CPU——
     * 解码线程仍在后台跑（停/启 V4L2 流有风险，不值得为省这点电去动），
     * 但至少别再往看不见的 lv_image 上刷帧。
     */
    if (!lv_obj_is_hidden(g_page_album)) return;

    uint32_t seq = 0;
    const uint8_t *frame = cam_preview_frame(&seq);
    if (frame == NULL) return;              /* 还没有新帧，保持上一帧画面 */

    g_img_dsc.data = frame;

    if (!g_src_set) {
        /* 首帧到了才设 src。在那之前 .data 是 NULL，
         * lv_image_set_src 里的 lv_image_decoder_get_info 拿不到图像信息会直接拒绝。 */
        lv_image_set_src(g_preview_img, &g_img_dsc);
        g_src_set = 1;
    }

    lv_obj_invalidate(g_preview_img);
}

/* 相册页的实现排在后面，这里先声明——on_gallery 要用 */
static void album_enter(void);

/*
 * 按钮回调。删除留到阶段 5——相册页右侧还有位置，届时加第四个圆钮即可。
 */
static void on_capture(lv_event_t *e)
{
    (void)e;
    /* 只置个标志就返回，不阻塞 UI；真正的截帧在解码线程里发生。
     * 这里不弹「保存中」提示——截帧发生在下一个解码帧（约 35ms 后），
     * 结果提示马上就来了，中间那句会一闪而过反而碍眼 */
    cam_preview_request_capture();
}

static void on_gallery(lv_event_t *e)
{
    (void)e;
    album_enter();
}

/*
 * 圆形按钮：LVGL 里没有专门的圆形控件，做法是正方形 + 圆角半径取一半
 * （LV_RADIUS_CIRCLE 就是「按短边取半」的语义），正方形配它才是正圆。
 *
 * icon 传 LV_SYMBOL_* 常量即可。
 */
static lv_obj_t *make_button(lv_obj_t *parent, const char *icon,
                             lv_event_cb_t cb, int32_t diameter)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, diameter, diameter);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    /* 默认内边距会把圆心挤偏，清掉让 label 真正居中 */
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

    /*
     * 全部按钮都浮在实时画面或照片上，必须半透明，否则一个实心圆把内容
     * 挡掉一块，而且画面明暗变化时纯色按钮会很突兀。
     * 描边保证在浅色画面上也看得见轮廓。
     *
     * 底色用纯黑而不是主题色：照片和预览的明暗跨度大，只有黑色能保证
     * 白色图标和白色描边在两种背景下都有足够对比度。
     */
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(btn, LV_OPA_40, LV_PART_MAIN);

    /*
     * 关掉阴影。
     *
     * LVGL 默认主题给按钮加的是「灰色、宽 3、50% 透明、**向下偏移 3px**」的
     * 投影（src/themes/default/lv_theme_default.c 的 styles.btn）。
     * 按钮浮在预览画面和照片上时，这圈影子就表现为下半边发虚、像重影。
     *
     * 主题里 radius/bg_opa/bg_color/pad 我们都已经覆盖，唯独漏了阴影这一项。
     */
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, icon);
    /* 图标字形在 Montserrat 里，必须显式指定这个字体，
     * 否则默认的 14px 会让 48px 的圆里只出现一个小点 */
    lv_obj_set_style_text_font(label, ICON_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(label);
    return btn;
}

/* -------------------------------------------------------------------------
 * 相册页
 * ------------------------------------------------------------------------- */

/* 显示第 g_index 张。索引越界会自动夹回范围 */
static void album_show_index(void)
{
    char path[PATH_MAX];
    int  w = 0, h = 0;
    const uint8_t *buf;

    /* 计数一直显示，空相册就是 0/0 */
    lv_label_set_text_fmt(g_album_label, "%d/%d",
                          g_count > 0 ? g_index + 1 : 0, g_count);

    if (g_index < 0)        g_index = 0;
    if (g_index >= g_count) g_index = g_count - 1;

    if (g_count <= 0) {
        /* 空相册。居中一句英文——屏幕上是拉丁字体，写不了汉字 */
        lv_obj_set_hidden(g_album_img, true);
        lv_label_set_text(g_album_hint, "No photos");
        lv_obj_set_hidden(g_album_hint, false);
        return;
    }

    photo_store_path(g_names[g_index], path, sizeof(path));

    buf = photo_view_open(path, g_photo_area_w, g_photo_area_h, &w, &h);
    if (buf == NULL) {
        /* 单张打不开不该让整个相册不可用，给句提示继续翻别的 */
        lv_obj_set_hidden(g_album_img, true);
        lv_label_set_text(g_album_hint, "Cannot open");
        lv_obj_set_hidden(g_album_hint, false);
        return;
    }

    /* 每张的尺寸都可能不同，所以 header 要重填、set_src 要重调——
     * LVGL 只在 lv_image_set_src 时读一次 w/h/cf，之后改 dsc 不生效 */
    g_album_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    g_album_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    g_album_dsc.header.w      = (uint32_t)w;
    g_album_dsc.header.h      = (uint32_t)h;
    g_album_dsc.header.stride = (uint32_t)w * 2;
    g_album_dsc.data_size     = (uint32_t)w * (uint32_t)h * 2;
    g_album_dsc.data          = buf;

    lv_image_set_src(g_album_img, &g_album_dsc);
    lv_obj_set_size(g_album_img, w, h);
    lv_obj_center(g_album_img);
    lv_obj_set_hidden(g_album_img, false);

    lv_obj_set_hidden(g_album_hint, true);
}

static void album_enter(void)
{
    /* 每次进来都重扫目录——上次进来之后可能又拍了新的 */
    g_count = photo_store_list(g_names, PHOTO_MAX_LIST);

    /* 从最新那张看起，符合「拍完马上看」的习惯。
     * 空相册时 g_index 会是 -1，album_show_index 里会先处理掉 */
    g_index = g_count - 1;

    album_show_index();
    lv_obj_set_hidden(g_page_album, false);

    if (g_count > 0)
        printf("[相册] 共 %d 张，从第 %d 张看起\n", g_count, g_index + 1);
    else
        printf("[相册] 目录里还没有照片\n");
    fflush(stdout);
}

/* 三个按钮的回调。翻页到头会绕回去，这样不用先判断方向能不能翻 */
static void on_album_prev(lv_event_t *e)
{
    (void)e;
    if (g_count <= 0) return;
    g_index--;
    if (g_index < 0) g_index = g_count - 1;
    album_show_index();
}

static void on_album_next(lv_event_t *e)
{
    (void)e;
    if (g_count <= 0) return;
    g_index++;
    if (g_index >= g_count) g_index = 0;
    album_show_index();
}

static void on_album_back(lv_event_t *e)
{
    (void)e;
    lv_obj_set_hidden(g_page_album, true);
}

/* 删除确认浮层的实现排在 album_build 之后，这里先声明 */
static void album_confirm_delete(void);

static void on_album_delete(lv_event_t *e)
{
    (void)e;
    album_confirm_delete();
}

/* 建相册页。parent 通常是 screen；整页盖满，默认隐藏 */
static void album_build(lv_obj_t *parent, int32_t W, int32_t H)
{
    /*
     * 照片铺满整屏，按钮浮在它上面——不再切出一条右侧按钮栏。
     * 这样照片能用满 480x272，代价是按钮会压住画面边角，
     * 所以按钮都做成半透明、描边，并且刻意贴四个角，少挡中间的内容。
     */
    g_photo_area_w = W;
    g_photo_area_h = H;

    g_page_album = lv_obj_create(parent);
    lv_obj_set_size(g_page_album, W, H);
    lv_obj_set_pos(g_page_album, 0, 0);
    lv_obj_set_style_bg_color(g_page_album, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_page_album, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_page_album, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_page_album, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_page_album, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(g_page_album, false);
    lv_obj_set_hidden(g_page_album, true);

    /* 照片。尺寸和内容每张都不一样，在 album_show_index 里填 */
    memset(&g_album_dsc, 0, sizeof(g_album_dsc));
    g_album_img = lv_image_create(g_page_album);
    lv_obj_set_hidden(g_album_img, true);

    /* 空相册 / 单张打不开时的提示：屏幕正中一个大图标 */
    g_album_hint = lv_label_create(g_page_album);
    lv_label_set_text(g_album_hint, "");
    lv_obj_set_style_text_font(g_album_hint, ICON_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_album_hint, lv_color_hex(0x506070), LV_PART_MAIN);
    lv_obj_center(g_album_hint);
    lv_obj_set_hidden(g_album_hint, true);

    /*
     * 计数「当前/总数」，放右上角——和左上角的返回键分踞两侧互不遮挡。
     * 用 ASCII 数字而不是「第 N / 共 M 张」，省掉一整套中文字体。
     */
    g_album_label = lv_label_create(g_page_album);
    lv_label_set_text(g_album_label, "0/0");
    lv_obj_set_style_bg_color(g_album_label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_album_label, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_album_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_album_label, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(g_album_label, 4, LV_PART_MAIN);
    lv_obj_align(g_album_label, LV_ALIGN_TOP_RIGHT, -BTN_MARGIN, BTN_MARGIN);

    /*
     * 四个按钮分踞四边：
     *   左上 返回   右上 计数   左中 上一张   右中 下一张   右下 删除
     * 图标用 LV_SYMBOL_*，字形取自 Montserrat 私用区
     */
    lv_obj_t *b;

    b = make_button(g_page_album, LV_SYMBOL_CLOSE, on_album_back, BTN_D);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, BTN_MARGIN, BTN_MARGIN);

    b = make_button(g_page_album, LV_SYMBOL_LEFT, on_album_prev, BTN_D);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, BTN_MARGIN, 0);

    b = make_button(g_page_album, LV_SYMBOL_RIGHT, on_album_next, BTN_D);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, -BTN_MARGIN, 0);

    b = make_button(g_page_album, LV_SYMBOL_TRASH, on_album_delete, BTN_D);
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -BTN_MARGIN, -BTN_MARGIN);
}

/* -------------------------------------------------------------------------
 * 删除确认浮层
 *
 * 为什么要二次确认：触摸屏误触很常见，而 unlink 不可撤销。
 * 为什么不用 lv_msgbox：它的按钮是文字按钮，而我们整套 UI 都是图标；
 * 自建一个浮层既风格统一，也少一个组件依赖。
 * ------------------------------------------------------------------------- */
static lv_obj_t *g_confirm;        /* 全屏遮罩，同时也是布局容器 */
static lv_obj_t *g_confirm_label;

static void confirm_close(void)
{
    lv_obj_set_hidden(g_confirm, true);
}

static void on_confirm_cancel(lv_event_t *e)
{
    (void)e;
    confirm_close();
}

static void on_confirm_delete(lv_event_t *e)
{
    (void)e;

    confirm_close();
    if (g_count <= 0) return;

    /* 删文件，然后重扫目录。不在内存列表里直接摘——重扫才能反映外部改动
     * （用户可能同时在串口里删文件），代价也不过一次 readdir */
    if (photo_store_delete(g_names[g_index]) != 0) {
        toast_show("Delete failed", 2000);
        return;
    }

    g_count = photo_store_list(g_names, PHOTO_MAX_LIST);

    /* 删掉第 i 张之后，原来的第 i+1 张会滑到位置 i，所以索引原地不动
     * 正好落在下一张上；删的是最后一张时索引越界，夹回末尾即可 */
    if (g_index >= g_count) g_index = g_count - 1;

    album_show_index();
    toast_show("Deleted", 1200);
}

static void album_confirm_delete(void)
{
    if (g_count <= 0) return;   /* 空相册没什么可删的，连浮层都不用弹 */

    lv_label_set_text_fmt(g_confirm_label, "Delete %s?", g_names[g_index]);
    lv_obj_set_hidden(g_confirm, false);
}

/* 建确认浮层。整屏半透明遮罩，默认隐藏 */
static void confirm_build(lv_obj_t *parent, int32_t W, int32_t H)
{
    g_confirm = lv_obj_create(parent);
    lv_obj_set_size(g_confirm, W, H);
    lv_obj_set_pos(g_confirm, 0, 0);
    lv_obj_set_style_bg_color(g_confirm, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_confirm, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_confirm, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_confirm, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_confirm, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(g_confirm, false);
    lv_obj_set_hidden(g_confirm, true);

    g_confirm_label = lv_label_create(g_confirm);
    lv_obj_set_style_text_font(g_confirm_label, ICON_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_confirm_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(g_confirm_label, LV_ALIGN_CENTER, 0, -62);

    /* 取消在左、确认在右。确认键染成红色——这是破坏性操作，
     * 和旁边那个「关闭」图标必须在视觉上能一眼分开 */
    lv_obj_t *b = make_button(g_confirm, LV_SYMBOL_CLOSE, on_confirm_cancel, BTN_D);
    lv_obj_align(b, LV_ALIGN_CENTER, -56, 16);

    b = make_button(g_confirm, LV_SYMBOL_OK, on_confirm_delete, BTN_D);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xA02020), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, LV_PART_MAIN);
    lv_obj_align(b, LV_ALIGN_CENTER, 56, 16);

    /* 按钮下方补一行文字。图标能省则省，但删除这件事值得写清楚 */
    lv_obj_t *t = lv_label_create(g_confirm);
    lv_label_set_text(t, "Cancel");
    lv_obj_align(t, LV_ALIGN_CENTER, -56, 56);

    t = lv_label_create(g_confirm);
    lv_label_set_text(t, "Delete");
    lv_obj_set_style_text_color(t, lv_color_hex(0xFF8080), LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_CENTER, 56, 56);
}

int main(int argc, char **argv)
{
    const char *fbdev = (argc > 1) ? argv[1] : DEF_FBDEV;
    const char *input = (argc > 2) ? argv[2] : DEF_INPUT;

    lv_init();

    /* ---------------- 显示 ---------------- */

    lv_display_t *disp = lv_linux_fbdev_create();
    if (disp == NULL) {
        fprintf(stderr, "错误: 创建 fbdev 显示失败\n");
        return 1;
    }

    if (lv_linux_fbdev_set_file(disp, fbdev) != LV_RESULT_OK) {
        fprintf(stderr, "错误: 初始化 %s 失败（上面有驱动的日志说明卡在哪）\n", fbdev);
        return 1;
    }

    /*
     * 一切尺寸都从这里来。fb0 报告多少就用多少——
     * 如果这里的数字和你的屏对不上，问题在内核/设备树，不在这个程序。
     */
    const int32_t W = lv_display_get_horizontal_resolution(disp);
    const int32_t H = lv_display_get_vertical_resolution(disp);

    printf("[显示] %s 已挂载，fb0 报告分辨率 %" LV_PRId32 "x%" LV_PRId32 "\n",
           fbdev, W, H);
    fflush(stdout);

    /* ---------------- 触摸 ---------------- */

    lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, input);
    if (touch == NULL) {
        fprintf(stderr,
                "错误: 打开 %s 失败 —— 换个 event 号试试，"
                "用 cat /proc/bus/input/devices 找触摸屏\n",
                input);
        return 1;
    }
    printf("[触摸] %s 已挂载\n", input);

    /* ---------------- UI ---------------- */

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(scr, false);

    /* 不用给 screen 设字体：界面文案已经全部换成图标或 ASCII，
     * 图标那一处由 make_button() 单独指定 ICON_FONT，其余走默认的 Montserrat 14 */

    /* 预览占满整屏。640x360 与 480x272 比例几乎一致，缩放后只裁 0.74% */
    const int32_t pf_w = W;
    const int32_t pf_h = H;

    lv_obj_t *preview = lv_obj_create(scr);
    lv_obj_set_size(preview, pf_w, pf_h);
    lv_obj_set_pos(preview, 0, 0);
    lv_obj_set_style_bg_color(preview, lv_color_hex(0x1E2530), LV_PART_MAIN);
    lv_obj_set_style_border_width(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(preview, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(preview, false);

    /*
     * 图像描述符：尺寸与预览区一致，格式 RGB565。
     * 三者对齐——摄像头解码输出 RGB565、这里 RGB565、fb0 是 16bpp——
     * 所以从解码到屏幕整条链路上没有任何像素格式转换。
     *
     * data 先留 NULL，等首帧到达再在 preview_timer_cb 里补上。
     */
    memset(&g_img_dsc, 0, sizeof(g_img_dsc));
    g_img_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    g_img_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    g_img_dsc.header.w      = (uint32_t)pf_w;
    g_img_dsc.header.h      = (uint32_t)pf_h;
    g_img_dsc.header.stride = (uint32_t)pf_w * 2;
    g_img_dsc.data_size     = (uint32_t)pf_w * (uint32_t)pf_h * 2;

    g_preview_img = lv_image_create(preview);
    lv_obj_set_size(g_preview_img, pf_w, pf_h);
    lv_obj_center(g_preview_img);

    /*
     * 两个圆形按钮浮在画面右侧、垂直居中。
     *
     * 直接挂在 scr 上（晚于 preview 创建，所以在画面之上），用绝对对齐而非
     * 容器 flex——浮动控件不该参与布局，否则它占的位置又会把预览挤小，
     * 那就退回原来的毛病了。
     */
    const int32_t half = (BTN_D + BTN_GAP) / 2;   /* 圆心相对屏幕中线的偏移 */

    /* 摄像机图标 + 图片图标，分别对应拍照和相册 */
    lv_obj_t *b1 = make_button(scr, LV_SYMBOL_VIDEO, on_capture, BTN_D);
    lv_obj_align(b1, LV_ALIGN_RIGHT_MID, -BTN_MARGIN, -half);

    lv_obj_t *b2 = make_button(scr, LV_SYMBOL_IMAGE, on_gallery, BTN_D);
    lv_obj_align(b2, LV_ALIGN_RIGHT_MID, -BTN_MARGIN, half);

    /* 相册页。整页盖满屏幕，默认隐藏——显示它就把相机页遮住了 */
    album_build(scr, W, H);

    /* 删除确认浮层，盖在相册页之上 */
    confirm_build(scr, W, H);

    /*
     * 拍照/删除的反馈浮层。
     *
     * 必须**最后**创建：LVGL 按创建顺序叠放，晚建的在上层。放在相册页之前
     * 的话会被相册页整个盖住，相册里删完照片那句「Deleted」就看不见了。
     */
    g_toast = lv_label_create(scr);
    lv_label_set_text(g_toast, "");
    lv_obj_set_style_text_font(g_toast, ICON_FONT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_toast, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_toast, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_toast, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_toast, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(g_toast, 6, LV_PART_MAIN);
    lv_obj_align(g_toast, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_set_hidden(g_toast, true);

    /* ---------------- 照片存储 ---------------- */

    /*
     * 目录不可用不该让整个程序挂掉——预览和触摸还能用，
     * 只是按下快门会提示保存失败，日志里也会说明原因。
     */
    const char *photo_dir = (argc > 4) ? argv[4] : DEF_PHOTO_DIR;

    if (photo_store_init(photo_dir) == 0) {
        printf("[存储] 照片目录: %s（现有 %d 张）\n",
               photo_store_dir(), photo_store_count());
    }
    else {
        fprintf(stderr, "错误: 照片目录不可用，拍照会失败\n");
    }

    /* ---------------- 摄像头 ---------------- */

    const char *v4l2 = (argc > 3) ? argv[3] : DEF_V4L2;

    if (cam_preview_start(v4l2, pf_w, pf_h, WANT_CAM_W, WANT_CAM_H) != 0) {
        /* 摄像头起不来不该让整个 UI 挂掉——按钮和触摸还能用，
         * 方便区分是相机的问题还是显示的问题。原因见上面的日志 */
        fprintf(stderr, "错误: 摄像头启动失败（%s），UI 继续运行但没有画面\n",
                v4l2);
    }
    else {
        g_preview_timer = lv_timer_create(preview_timer_cb, PREVIEW_PERIOD_MS, NULL);
    }

    printf("[UI] 就绪，等待触摸...\n");
    fflush(stdout);

    /* ---------------- 主循环 ---------------- */

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    while (!g_quit) {
        lv_timer_handler();
        usleep(5 * 1000); /* 5ms，够用且不空转烧 CPU */
    }

    /* 顺序收摊：先停采集与解码线程，再让 LVGL 收尾 */
    printf("\n[退出] 停止摄像头...\n");
    cam_preview_stop();

    /*
     * 把屏幕擦干净再退出。
     *
     * 不擦的话 LCD 会一直停着最后一帧预览画面，看上去像程序还在跑、
     * 或者像死机了。擦成黑屏最明确：程序已经退了。
     *
     * 借 LVGL 自己重绘——清掉所有控件、把底色设成纯黑、强制立刻刷新一次，
     * 比另外开一遍 /dev/fb0 走 mmap 简单得多。
     *
     * 定时器必须先删：preview_timer_cb 会碰 g_preview_img / g_page_album，
     * 而 lv_obj_clean() 会把它们全释放掉，留着就是个悬空指针。
     * （清完之后我们不再调 lv_timer_handler，所以其它一次性定时器
     *   即使还挂着也不会触发。）
     */
    if (g_preview_timer != NULL) {
        lv_timer_delete(g_preview_timer);
        g_preview_timer = NULL;
    }

    /* scr 就是上面 UI 段里那个 lv_screen_active()，直接复用 */
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_refr_now(NULL);
    printf("[退出] 已释放 %s\n", v4l2);
    return 0;
}
