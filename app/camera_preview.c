#include "camera_preview.h"

#include "camera/capture.h"
#include "camera/decoder.h"
#include "camera/frame_queue.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 三缓冲：发布后要再写两帧才回到同一块，给了 2 个帧周期的余量，避免撕裂 */
#define CAM_BUF_COUNT 3

/* 3 帧足够吸收解码抖动；满了走 FQ_DROP_OLDEST 丢旧留新 */
#define CAM_QUEUE_FRAMES 3

/* 缩放后上下留边那几行铺的底色，RGB565 */
#define BACKDROP_RGB565 0x18A3u

/* 最大等待时间，也是解码线程检查 running 标志的周期 */
#define POP_TIMEOUT_MS 100

static struct {
    capture     *cap;
    frame_queue *fq;
    decoder     *dec;

    int          out_w, out_h;        /* 输出到屏幕的尺寸 */
    int          cam_w, cam_h;        /* 摄像头实际分辨率（协商后的） */
    pixel_format fmt;

    /* 等比缩放后的画面尺寸与居中偏移。启动时按 out/cam 尺寸算一次，之后不变 */
    int          disp_w, disp_h;
    int          off_x, off_y;
    /* 输出列 → 源列 的映射表（disp_w 项）。逐像素做除法太贵，启动时查表算好 */
    uint16_t    *xmap;

    uint8_t     *dst[CAM_BUF_COUNT];  /* RGB565 输出三缓冲 */
    uint8_t     *decoded;             /* 解码输出缓冲，RGB565 */
    size_t       decoded_size;

    int          write_idx;           /* 生产端正在写的那块 */
    int          published;           /* 刚写完、可显示的；-1 = 还没有 */
    uint32_t     seq;                 /* 已发布帧计数 */
    uint32_t     shown_seq;           /* 消费端已取走的帧计数 */

    /* 拍照：capture_req 由 UI 线程置、解码线程清；capture_ready 表示有帧待取 */
    uint8_t     *capture_buf;
    size_t       capture_size;
    volatile int capture_req;
    int          capture_ready;

    pthread_mutex_t lock;
    pthread_t    thread;
    int          running;

    /* 统计，解码线程每秒刷新一次。用来判断瓶颈在解码还是在显示 */
    double       fps;
    double       cost_ms;             /* 单帧 解码+转换 的平均耗时 */
} g;

static int g_inited = 0;

/* 单调时钟秒数，不受系统改时间影响 */
static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* 等比缩放填充
 *
 * 用最近邻：本来就要逐像素过一遍，改成缩放后循环次数不变，只是源索引变成查表。
 * 双线性要多做 4 邻域加权，A7 上是三四倍开销。640→480 是缩小，边缘可能有锯齿。
 *
 * 输入已经是 RGB565，与 LVGL 的布局一致，所以这里不碰颜色 */
static void scale_into(const uint8_t *src, int src_w, int src_h,
                          uint8_t *dst, int dst_w, int dst_h)
{
    uint16_t *d = (uint16_t *)dst;

    /* 先铺满背景色，四周不足的边就自动是它了 */
    for (int i = 0; i < dst_w * dst_h; i++) d[i] = BACKDROP_RGB565;

    for (int y = 0; y < g.disp_h; y++) {
        const int sy = (int)((int64_t)y * src_h / g.disp_h);
        const uint16_t *srow = (const uint16_t *)(src + (size_t)sy * src_w * 2);
        uint16_t *drow = d + (size_t)(y + g.off_y) * dst_w + g.off_x;

        for (int x = 0; x < g.disp_w; x++)
            *drow++ = srow[g.xmap[x]];
    }
}

/* 解码线程：唯一调用 libjpeg 的地方，绝不碰任何 lv_* 函数 */
static void *decode_thread(void *arg)
{
    (void)arg;

    const size_t raw_cap = cap_max_frame_size(g.cap);
    uint8_t *raw = malloc(raw_cap);
    if (!raw) {
        fprintf(stderr, "[预览] 原始帧缓冲分配失败（%zu 字节）\n", raw_cap);
        return NULL;
    }

    unsigned long decoded = 0, failed = 0;
    unsigned long win_frames = 0;
    double        win_cost = 0.0;
    double        win_start = now_sec();

    while (g.running) {
        size_t sz = raw_cap;

        /* 超时返回 -1，正好用来周期性回查 running，不必用条件变量 */
        if (fq_pop(g.fq, raw, &sz, POP_TIMEOUT_MS) != 0) continue;

        /* 拍照截帧：必须在这里做，队列是解码线程独占消费的。
         * memcpy 20KB 只要几十微秒 */
        if (g.capture_req) {
            memcpy(g.capture_buf, raw, sz);
            pthread_mutex_lock(&g.lock);
            g.capture_size  = sz;
            g.capture_ready = 1;
            pthread_mutex_unlock(&g.lock);
            g.capture_req = 0;
        }

        const double t0 = now_sec();

        if (decoder_decode(g.dec, raw, sz, g.decoded, g.decoded_size) != 0) {
            failed++;
            continue;   /* 单帧损坏不该拖垮整个预览，丢掉继续 */
        }

        const int w = g.write_idx;
        scale_into(g.decoded, g.cam_w, g.cam_h, g.dst[w], g.out_w, g.out_h);

        const double t1 = now_sec();
        win_cost  += (t1 - t0);
        win_frames++;

        pthread_mutex_lock(&g.lock);
        g.published = w;
        g.seq++;
        pthread_mutex_unlock(&g.lock);

        g.write_idx = (w + 1) % CAM_BUF_COUNT;
        decoded++;

        /* 每秒报一次，帧率是后续取舍的依据 */
        if (t1 - win_start >= 1.0) {
            g.fps     = win_frames / (t1 - win_start);
            g.cost_ms = win_frames ? win_cost * 1000.0 / win_frames : 0.0;
            printf("[预览] %.1f fps | 解码+转换 %.1f ms/帧 | 队列丢帧 %lu\n",
                   g.fps, g.cost_ms, fq_dropped(g.fq));
            fflush(stdout);
            win_frames = 0;
            win_cost   = 0.0;
            win_start  = t1;
        }
    }

    printf("[预览] 解码线程退出：成功 %lu 帧，失败 %lu 帧\n", decoded, failed);
    free(raw);
    return NULL;
}

/* ------------------------------------------------------------------------- */

int cam_preview_start(const char *device, int out_w, int out_h,
                      int want_w, int want_h)
{
    if (g_inited) return -1;
    if (out_w <= 0 || out_h <= 0) return -1;

    memset(&g, 0, sizeof(g));
    g.published = -1;
    g.out_w = out_w;
    g.out_h = out_h;

    capture_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.device       = device;
    cfg.fmt          = PIX_FMT_MJPEG;
    cfg.width        = (unsigned)want_w;
    cfg.height       = (unsigned)want_h;
    cfg.buffer_count = 4;

    printf("[预览] 打开 %s，请求 %dx%d MJPEG\n", device, want_w, want_h);

    g.cap = cap_create(&cfg);
    if (!g.cap) {
        fprintf(stderr, "[预览] cap_create 失败\n");
        return -1;
    }

    /* 驱动可能改掉请求值，一律以读回的为准 */
    g.fmt   = cap_format(g.cap);
    g.cam_w = (int)cap_width(g.cap);
    g.cam_h = (int)cap_height(g.cap);

    printf("[预览] 实际协商: %dx%d %s\n", g.cam_w, g.cam_h,
           pixel_format_name(g.fmt));

    if (g.fmt != PIX_FMT_MJPEG) {
        printf("[预览] 注意: 驱动没给 MJPEG，是 %s。预览能跑，"
               "但阶段 3 拍照时存原始帧就不是 JPEG 了\n",
               pixel_format_name(g.fmt));
    }
    /* 等比缩放到刚好放进输出区，再居中。先按铺满宽试算，放不下改成铺满高 */
    if ((int64_t)g.out_w * g.cam_h <= (int64_t)g.out_h * g.cam_w) {
        g.disp_w = g.out_w;                                          /* 宽度受限 */
        g.disp_h = (int)((int64_t)g.out_w * g.cam_h / g.cam_w);
    } else {
        g.disp_h = g.out_h;                                          /* 高度受限 */
        g.disp_w = (int)((int64_t)g.out_h * g.cam_w / g.cam_h);
    }
    g.off_x = (g.out_w - g.disp_w) / 2;
    g.off_y = (g.out_h - g.disp_h) / 2;

    const double fill = 100.0 * g.disp_w * g.disp_h / (g.out_w * g.out_h);
    printf("[预览] 缩放 %dx%d → %dx%d 居中 (偏移 %d,%d)，填充率 %.1f%%\n",
           g.cam_w, g.cam_h, g.disp_w, g.disp_h, g.off_x, g.off_y, fill);

    g.xmap = malloc(sizeof(uint16_t) * (size_t)g.disp_w);
    if (!g.xmap) {
        fprintf(stderr, "[预览] 缩放映射表分配失败\n");
        goto fail_cap;
    }
    for (int x = 0; x < g.disp_w; x++)
        g.xmap[x] = (uint16_t)((int64_t)x * g.cam_w / g.disp_w);

    g.dec = decoder_create(g.fmt, (unsigned)g.cam_w, (unsigned)g.cam_h);
    if (!g.dec) {
        fprintf(stderr, "[预览] decoder_create 失败\n");
        goto fail_cap;
    }

    g.decoded_size = decoder_output_size(g.dec);
    g.decoded = malloc(g.decoded_size);
    if (!g.decoded) {
        fprintf(stderr, "[预览] 解码缓冲分配失败（%zu 字节）\n", g.decoded_size);
        goto fail_dec;
    }

    /* 拍照缓冲：按 QUERYBUF 报的上界分配——MJPEG 每帧长度都不同 */
    const size_t maxf = cap_max_frame_size(g.cap);
    g.capture_buf = malloc(maxf);
    if (!g.capture_buf) {
        fprintf(stderr, "[预览] 拍照缓冲分配失败（%zu 字节）\n", maxf);
        goto fail_dec;
    }

    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        size_t n = (size_t)g.out_w * g.out_h * 2;
        g.dst[i] = malloc(n);
        if (!g.dst[i]) {
            fprintf(stderr, "[预览] 输出缓冲 %d 分配失败（%zu 字节）\n", i, n);
            goto fail_bufs;
        }
        memset(g.dst[i], 0, n);
    }

    /* 每槽都要装得下 maxf，否则 fq_push 会静默丢帧 */
    g.fq = fq_create(CAM_QUEUE_FRAMES, maxf, FQ_DROP_OLDEST);
    if (!g.fq) {
        fprintf(stderr, "[预览] 队列创建失败（%d 槽 × %zu 字节）\n",
                CAM_QUEUE_FRAMES, maxf);
        goto fail_bufs;
    }

    printf("[预览] 内存: 输出 %d×%d×2×%d = %zuKB, 解码缓冲 %zuKB, 队列 %zuKB\n",
           g.out_w, g.out_h, CAM_BUF_COUNT,
           (size_t)g.out_w * g.out_h * 2 * CAM_BUF_COUNT / 1024,
           g.decoded_size / 1024, CAM_QUEUE_FRAMES * maxf / 1024);

    if (pthread_mutex_init(&g.lock, NULL) != 0) {
        fprintf(stderr, "[预览] 互斥锁初始化失败\n");
        goto fail_fq;
    }

    /* cap_start 会起 libcamera 自己的采集线程，往队列里 push */
    if (cap_start(g.cap, g.fq) != 0) {
        fprintf(stderr, "[预览] cap_start 失败\n");
        goto fail_lock;
    }

    g.running = 1;
    if (pthread_create(&g.thread, NULL, decode_thread, NULL) != 0) {
        fprintf(stderr, "[预览] 解码线程创建失败\n");
        g.running = 0;
        cap_stop(g.cap);
        goto fail_lock;
    }

    g_inited = 1;
    printf("[预览] 已启动\n");
    return 0;

fail_lock:
    pthread_mutex_destroy(&g.lock);
fail_fq:
    fq_destroy(g.fq);
    g.fq = NULL;
fail_bufs:
    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        free(g.dst[i]);
        g.dst[i] = NULL;
    }
    free(g.decoded);
    g.decoded = NULL;
fail_dec:
    /* xmap / capture_buf 在这里统一回收；未分配时 free(NULL) 也安全 */
    free(g.xmap);
    g.xmap = NULL;
    free(g.capture_buf);
    g.capture_buf = NULL;
    decoder_destroy(g.dec);
    g.dec = NULL;
fail_cap:
    cap_destroy(g.cap);
    g.cap = NULL;
    return -1;
}

void cam_preview_stop(void)
{
    if (!g_inited) return;

    /* 先清 running 再 join：解码线程靠 fq_pop 超时周期性回查这个标志 */
    g.running = 0;
    pthread_join(g.thread, NULL);

    cap_stop(g.cap);        /* STREAMOFF + join libcamera 的采集线程 */
    cap_destroy(g.cap);

    pthread_mutex_destroy(&g.lock);
    fq_destroy(g.fq);
    decoder_destroy(g.dec);

    for (int i = 0; i < CAM_BUF_COUNT; i++) free(g.dst[i]);
    free(g.decoded);
    free(g.xmap);
    free(g.capture_buf);

    g_inited = 0;
}

void cam_preview_request_capture(void)
{
    if (!g_inited) return;
    g.capture_req = 1;
}

size_t cam_preview_take_captured(const uint8_t **data)
{
    size_t n = 0;

    if (!g_inited || !data) return 0;

    pthread_mutex_lock(&g.lock);
    if (g.capture_ready) {
        n = g.capture_size;
        *data = g.capture_buf;
        /* 交给调用方。解码线程在下一次新请求生效前不会再写这块缓冲 */
        g.capture_ready = 0;
        g.capture_size  = 0;
    }
    pthread_mutex_unlock(&g.lock);

    return n;
}

const uint8_t *cam_preview_frame(uint32_t *seq)
{
    const uint8_t *p = NULL;

    if (!g_inited) return NULL;

    pthread_mutex_lock(&g.lock);
    if (g.published >= 0 && g.seq != g.shown_seq) {
        p = g.dst[g.published];
        g.shown_seq = g.seq;
        if (seq) *seq = g.seq;
    }
    pthread_mutex_unlock(&g.lock);

    return p;
}
