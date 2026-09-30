/*
 * 阶段 2：实时预览
 *
 * 已通：LVGL 画到 /dev/fb0、触摸从 /dev/input/eventX 进来、摄像头实时画面。
 * 三个按钮暂时只打印，阶段 3~5 分别接上拍照 / 相册 / 删除。
 *
 * 布局全部按显示驱动报告的实际分辨率算，不写死尺寸——
 * 换一块屏不用改代码，也不会出现按钮跑到屏幕外面的情况。
 *
 * 线程模型（见 camera_preview.h 的说明）：
 *   libcamera 采集线程 → 环形队列 → 本进程解码线程 → 三缓冲 → LVGL 主线程
 * 解码线程绝不碰 lv_*，两边只交换一个裸缓冲指针。
 *
 * 用法：./lvgl_camera [fbdev] [input_event] [video_dev]
 *       默认 /dev/fb0、/dev/input/event1、/dev/video0
 *       触摸没反应就换个 event 号，见 /proc/bus/input/devices
 */
#include "lvgl.h"
#include "camera_preview.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define DEF_FBDEV  "/dev/fb0"
#define DEF_INPUT  "/dev/input/event1"
#define DEF_V4L2   "/dev/video0"

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
 * 中文字体子集，由 tools/gen_font.sh 生成（16px / 4bpp）。
 *
 * 为什么不用 LVGL 内置的那两款 CJK 字体：它们并不是完整的思源黑体，
 * 实测只覆盖 1450 个码点、其中汉字仅 297 个，本项目 UI 用字一个都不在
 * 里面，直接就是满屏方框。所以自己抽一份。
 *
 * 加新文案后如果出现方框：把新字补进 tools/gen_font.sh 的 SYMBOLS，
 * 重跑该脚本，再重新构建。脚本自带校验，缺字会当场报错。
 */
LV_FONT_DECLARE(font_zh_16);

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

/*
 * 按钮回调，现在只打印。阶段 3/4 分别接上拍照和相册。
 * 「删除」按钮阶段 5 再加回来——侧栏还有空间，届时三个圆形键重排即可。
 */
static void on_capture(lv_event_t *e)
{
    (void)e;
    printf("[按钮] 拍照（阶段 3 实现）\n");
    fflush(stdout);
}

static void on_gallery(lv_event_t *e)
{
    (void)e;
    printf("[按钮] 相册（阶段 4 实现）\n");
    fflush(stdout);
}

/*
 * 圆形按钮：LVGL 里没有专门的圆形控件，做法是正方形 + 圆角半径取一半
 * （LV_RADIUS_CIRCLE 就是「按短边取半」的语义），正方形配它才是正圆。
 */
static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             lv_event_cb_t cb, int32_t diameter)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, diameter, diameter);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    /* 默认内边距会把圆心挤偏，清掉让 label 真正居中 */
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

    /*
     * 浮在实时画面上，必须半透明，否则一个实心圆把画面挡掉一块，
     * 而且画面明暗变化时纯色按钮会显得很突兀。
     * 描边保证在浅色画面上也看得见轮廓。
     */
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(btn, LV_OPA_50, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
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
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * 给 screen 设字体，靠 LVGL 的样式继承覆盖底下所有控件，
     * 不用逐个 label 设。设的必须是字体结构体的地址。
     */
    lv_obj_set_style_text_font(scr, &font_zh_16, LV_PART_MAIN);

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
    lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLLABLE);

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

    lv_obj_t *b1 = make_button(scr, "拍照", on_capture, BTN_D);
    lv_obj_align(b1, LV_ALIGN_RIGHT_MID, -BTN_MARGIN, -half);

    lv_obj_t *b2 = make_button(scr, "相册", on_gallery, BTN_D);
    lv_obj_align(b2, LV_ALIGN_RIGHT_MID, -BTN_MARGIN, half);

    /* ---------------- 摄像头 ---------------- */

    const char *v4l2 = (argc > 3) ? argv[3] : DEF_V4L2;

    if (cam_preview_start(v4l2, pf_w, pf_h, WANT_CAM_W, WANT_CAM_H) != 0) {
        /* 摄像头起不来不该让整个 UI 挂掉——按钮和触摸还能用，
         * 方便区分是相机的问题还是显示的问题。原因见上面的日志 */
        fprintf(stderr, "错误: 摄像头启动失败（%s），UI 继续运行但没有画面\n",
                v4l2);
    }
    else {
        lv_timer_create(preview_timer_cb, PREVIEW_PERIOD_MS, NULL);
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
    printf("[退出] 已释放 %s\n", v4l2);
    return 0;
}
