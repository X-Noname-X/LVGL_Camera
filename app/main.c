/*
 * 拍照走零编码路径：每个 MJPEG 帧本身就是一张完整 JPEG，直接写文件即可，
 * 没有编码开销和画质损失，存下来还是摄像头原始分辨率。
 *
 * 布局全部按 fb0 报告的实际分辨率算，不写死尺寸。
 *
 * 采集线程 → 环形队列 → 解码线程 → 三缓冲 → LVGL 主线程
 * 解码线程绝不碰 lv_*，两边只交换一个裸缓冲指针。
 *
 * 用法：./lvgl_camera [fbdev] [input_event] [video_dev] [photo_dir]
 *   /dev/fb0  /dev/input/event1  /dev/video2  /run/media/mmcblk0p1/photos
 */
#include "lvgl.h"
#include "camera_preview.h"
#include "photo_store.h"
#include "photo_view.h"

#include <limits.h>   /* PATH_MAX */
#include <signal.h>
#include <stdio.h>
#include <string.h>   /* memset */
#include <unistd.h>

#define DEF_FBDEV  "/dev/fb0"
#define DEF_INPUT  "/dev/input/event1"
#define DEF_V4L2   "/dev/video2"
#define DEF_PHOTO_DIR "/run/media/mmcblk0p1/photos"

/* 640x360 与屏幕 480x272 比例几乎一致；代价是解码像素数变成 3 倍 */
#define WANT_CAM_W 640
#define WANT_CAM_H 360

/* 浮动圆钮*/
#define BTN_D      48
#define BTN_MARGIN 12
#define BTN_GAP    16

/* 取帧周期约 30fps，真正的帧率由采集端决定 */
#define PREVIEW_PERIOD_MS 33

/* 相册一次最多列多少张。128 个文件名才 4KB，用固定数组省掉内存管理 */
#define PHOTO_MAX_LIST 128

/* 图标字体。LV_SYMBOL_xxx 当普通字符串传给 lv_label_set_text() 即可 */
#define ICON_FONT (&lv_font_montserrat_24)

// Ctrl+C 退出标志
static volatile sig_atomic_t g_quit = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

// 预览用的图像描述符
static lv_image_dsc_t g_img_dsc;
static lv_obj_t      *g_preview_img;
static int            g_src_set;
static lv_timer_t    *g_preview_timer;   /* 退出前要停掉它再清屏 */

/* 相册页。用「全屏不透明容器盖住」而不是 LVGL 的多 screen——显示它就把
 * 相机页整个遮住，代价是预览定时器要跳过刷新（见 preview_timer_cb） */
static lv_obj_t      *g_page_album;      /* 全屏容器，默认隐藏 */
static lv_obj_t      *g_album_img;       /* 照片 */
static lv_obj_t      *g_album_label;     /* 第 N / 共 M 张 */
static lv_image_dsc_t g_album_dsc;       /* 指向 photo_view 的内部缓冲 */

static char g_names[PHOTO_MAX_LIST][PHOTO_NAME_MAX];
static int  g_count;                     /* 当前列表里有多少张 */
static int  g_index;                     /* 正在看第几张（0 起） */
static int  g_photo_area_w, g_photo_area_h;   /* 照片可用区尺寸 */

// 浮层提示：显示一小会儿自动隐藏
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
    // 设 1 = 一次性，跑完自删
    lv_timer_set_repeat_count(lv_timer_create(toast_hide_cb, ms, NULL), 1);
}

/* 定时取帧。这是 LVGL 主线程，也是唯一允许碰 lv_* 的地方。
 *
 * 指针指向三缓冲之一，生产端要再写两帧才回到同一块，渲染期间不会被覆盖。
 * 每帧只改 .data 再 invalidate，不重建控件也不重复 set_src——LVGL 对
 * 未压缩格式直接使用这个指针，不拷贝 */
static void preview_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* 先处理拍照：写 20KB 到 flash 要 10~50ms，但用户刚按下快门，
     * 这点停顿符合预期。放在取帧之前，队列超时期间也能及时存下来 */
    const uint8_t *jpeg = NULL;
    size_t n = cam_preview_take_captured(&jpeg);
    if (n > 0) {
        char name[64];
        if (photo_store_save(jpeg, n, name, sizeof(name)) == 0) {
            char msg[96];
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

    /* 相册页盖住了预览，别往看不见的 lv_image 上白刷帧 */
    if (!lv_obj_is_hidden(g_page_album)) return;

    uint32_t seq = 0;
    const uint8_t *frame = cam_preview_frame(&seq);
    if (frame == NULL) return;              /* 还没有新帧，保持上一帧画面 */

    g_img_dsc.data = frame;

    if (!g_src_set) {
        /* 首帧之前 .data 是 NULL，set_src 拿不到图像信息会直接拒绝 */
        lv_image_set_src(g_preview_img, &g_img_dsc);
        g_src_set = 1;
    }

    lv_obj_invalidate(g_preview_img);
}

static void album_enter(void);

static void on_capture(lv_event_t *e)
{
    (void)e;
    // 只置标志不阻塞 UI，真正截帧在解码线程里
    cam_preview_request_capture();
}

static void on_gallery(lv_event_t *e)
{
    (void)e;
    album_enter();
}

// 圆形按钮 = 正方形 + 圆角半径取一半
static lv_obj_t *make_button(lv_obj_t *parent, const char *icon,
                             lv_event_cb_t cb, int32_t diameter)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, diameter, diameter);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    /* 默认内边距会把圆心挤偏 */
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

    //全部按钮都浮在实时画面或照片上
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(btn, LV_OPA_40, LV_PART_MAIN);

    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);  //关掉阴影

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, icon);
    lv_obj_set_style_text_font(label, ICON_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(label);
    return btn;
}

/* 显示第 g_index 张 */
static void album_show_index(void)
{
    char path[PATH_MAX];
    int  w = 0, h = 0;
    const uint8_t *buf;

    // 计数一直显示，空相册就是 0/0
    lv_label_set_text_fmt(g_album_label, "%d/%d",
                          g_count > 0 ? g_index + 1 : 0, g_count);

    if (g_count <= 0) {
        //空相册
        lv_obj_set_hidden(g_album_img, true);
        return;
    }

    photo_store_path(g_names[g_index], path, sizeof(path));

    buf = photo_view_open(path, g_photo_area_w, g_photo_area_h, &w, &h);
    if (buf == NULL) {
        // 单张打不开不该让整个相册不可用
        lv_obj_set_hidden(g_album_img, true);
        toast_show("Cannot open", 1500);
        return;
    }

    /* 换照片要重填再 set_src：LVGL 只在 set_src 时读一次 w/h/cf */
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
}

static void album_enter(void)
{
    // 每次进来都重扫目录
    g_count = photo_store_list(g_names, PHOTO_MAX_LIST);

    // 从最新那张看起；空相册时是 -1，album_show_index 里会处理
    g_index = g_count - 1;

    album_show_index();
    lv_obj_set_hidden(g_page_album, false);
}

/* 翻页到头会绕回去，这样不用先判断方向能不能翻 */
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

/* 实现在 album_build 之后，先声明 */
static void album_confirm_delete(void);

static void on_album_delete(lv_event_t *e)
{
    (void)e;
    album_confirm_delete();
}

/* 建相册页：parent 通常是 screen；整页盖满，默认隐藏 */
static void album_build(lv_obj_t *parent, int32_t W, int32_t H)
{
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

    /* 尺寸和内容每张都不同，在 album_show_index 里填 */
    memset(&g_album_dsc, 0, sizeof(g_album_dsc));
    g_album_img = lv_image_create(g_page_album);
    lv_obj_set_hidden(g_album_img, true);

    //计数「当前/总数」，放右上角
    g_album_label = lv_label_create(g_page_album);
    lv_label_set_text(g_album_label, "0/0");
    lv_obj_set_style_bg_color(g_album_label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_album_label, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_album_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_album_label, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(g_album_label, 4, LV_PART_MAIN);
    lv_obj_align(g_album_label, LV_ALIGN_TOP_RIGHT, -BTN_MARGIN, BTN_MARGIN);

    /* 左上 返回 / 左中 上一张 / 右中 下一张 / 右下 删除（右上角是计数） */
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

// 删除确认浮层
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

    // 删文件，然后重扫目录
    if (photo_store_delete(g_names[g_index]) != 0) {
        toast_show("Delete failed", 2000);
        return;
    }

    g_count = photo_store_list(g_names, PHOTO_MAX_LIST);

    /* 第 i+1 张会滑到位置 i，索引原地不动正好落在下一张；
     * 删的是最后一张时越界，夹回末尾 */
    if (g_index >= g_count) g_index = g_count - 1;

    album_show_index();
    toast_show("Deleted", 1200);
}

static void album_confirm_delete(void)
{
    if (g_count <= 0) return;   // 空相册没什么可删的，连浮层都不用弹

    lv_label_set_text_fmt(g_confirm_label, "Delete %s?", g_names[g_index]);
    lv_obj_set_hidden(g_confirm, false);
}

/* 建确认浮层：整屏半透明遮罩，默认隐藏 */
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

    /* 取消在左、确认在右 确认键染成红色 */
    lv_obj_t *b = make_button(g_confirm, LV_SYMBOL_CLOSE, on_confirm_cancel, BTN_D);
    lv_obj_align(b, LV_ALIGN_CENTER, -56, 16);

    b = make_button(g_confirm, LV_SYMBOL_OK, on_confirm_delete, BTN_D);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xA02020), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, LV_PART_MAIN);
    lv_obj_align(b, LV_ALIGN_CENTER, 56, 16);

    /* 按钮下方补一行文字 */
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

    // 尺寸全部取自 fb0 的报告值；对不上屏就是内核/设备树的问题，不是这里
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

    /* 预览占满整屏  640x360 与 480x272 比例几乎一致，只裁不到 1% */
    const int32_t pf_w = W;
    const int32_t pf_h = H;

    /* 尺寸与预览区一致，RGB565。解码输出、这里、fb0(16bpp) 三者对齐，
     * 整条链路上没有一次像素格式转换。data 先留 NULL，首帧到达时再补 */
    memset(&g_img_dsc, 0, sizeof(g_img_dsc));
    g_img_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    g_img_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    g_img_dsc.header.w      = (uint32_t)pf_w;
    g_img_dsc.header.h      = (uint32_t)pf_h;
    g_img_dsc.header.stride = (uint32_t)pf_w * 2;
    g_img_dsc.data_size     = (uint32_t)pf_w * (uint32_t)pf_h * 2;

    g_preview_img = lv_image_create(scr);
    lv_obj_set_size(g_preview_img, pf_w, pf_h);
    lv_obj_center(g_preview_img);

    const int32_t half = (BTN_D + BTN_GAP) / 2;   // 圆心相对屏幕中线的偏移

    /* 摄像机图标 + 图片图标，分别对应拍照和相册 */
    lv_obj_t *b1 = make_button(scr, LV_SYMBOL_VIDEO, on_capture, BTN_D);
    lv_obj_align(b1, LV_ALIGN_RIGHT_MID, -BTN_MARGIN, -half);

    lv_obj_t *b2 = make_button(scr, LV_SYMBOL_IMAGE, on_gallery, BTN_D);
    lv_obj_align(b2, LV_ALIGN_RIGHT_MID, -BTN_MARGIN, half);

    album_build(scr, W, H); // 相册页。整页盖满屏幕，默认隐藏——显示它就把相机页遮住了
    confirm_build(scr, W, H);  // 删除确认浮层，盖在相册页之上

    // 拍照/删除的反馈浮层
    g_toast = lv_label_create(scr);
    lv_obj_set_style_text_font(g_toast, ICON_FONT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_toast, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_toast, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_toast, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_toast, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(g_toast, 6, LV_PART_MAIN);
    lv_obj_align(g_toast, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_set_hidden(g_toast, true);

    /* ---------------- 照片存储 ---------------- */
    // 目录不可用不该让程序挂掉：预览和触摸还能用，只是拍照会失败
    const char *photo_dir = (argc > 4) ? argv[4] : DEF_PHOTO_DIR;

    if (photo_store_init(photo_dir) == 0) {
        printf("[存储] 照片目录: %s（现有 %d 张）\n", photo_store_dir(), photo_store_count());
    }
    else {
        fprintf(stderr, "错误: 照片目录不可用，拍照会失败\n");
    }

    /* ---------------- 摄像头 ---------------- */

    const char *v4l2 = (argc > 3) ? argv[3] : DEF_V4L2;

    if (cam_preview_start(v4l2, pf_w, pf_h, WANT_CAM_W, WANT_CAM_H) != 0) {
        // 摄像头起不来不该让整个 UI 挂掉——按钮和触摸还能用
        fprintf(stderr, "错误: 摄像头启动失败（%s），UI 继续运行但没有画面\n", v4l2);
    }
    else {
        g_preview_timer = lv_timer_create(preview_timer_cb, PREVIEW_PERIOD_MS, NULL);
    }
    /* ---------------- 主循环 ---------------- */

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    while (!g_quit) {
        lv_timer_handler();
        usleep(5 * 1000); /* 5ms，够用且不空转烧 CPU */
    }

    /* 先停采集与解码线程，再让 LVGL 收尾 */
    printf("\n[退出] 停止摄像头...\n");
    cam_preview_stop();

    // 把屏幕擦干净再退出
    if (g_preview_timer != NULL) {
        lv_timer_delete(g_preview_timer);
        g_preview_timer = NULL;
    }
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_refr_now(NULL);
    return 0;
}
