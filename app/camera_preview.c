#include "camera_preview.h"

#include "camera/capture.h"
#include "camera/decoder.h"
#include "camera/frame_queue.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 输出缓冲个数。3 个是为了让生产端在消费端用完一帧之前不会绕回来覆盖它：
 * 发布第 N 帧后，生产端要再写两帧才回到同一块，中间给了整整 2 个帧周期。
 * 2 个缓冲在 LVGL 渲染偶尔卡一下时会撕裂，多这 195KB 换掉那个隐患很划算。 */
#define CAM_BUF_COUNT 3

/* 队列里缓存几帧。预览要的是「最新画面」，排太深只会徒增延迟，
 * 3 帧足够吸收解码抖动；满了走 FQ_DROP_OLDEST 丢旧留新。 */
#define CAM_QUEUE_FRAMES 3

/* 留边时铺的背景色，RGB565。和 UI 预览区底色一致 */
#define BACKDROP_RGB565 0x18A3u   /* 约 #1E2530 */

/* RGB888 → RGB565：各通道取高位打包 */
#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

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
    uint8_t     *decoded;             /* 解码输出缓冲：RGB565(2B/px) 或 RGB24(3B/px) */
    size_t       decoded_size;
    int          src_565;             /* 解码输出是否已经是 RGB565 */

    int          write_idx;           /* 生产端正在写的那块 */
    int          published;           /* 刚写完、可显示的；-1 = 还没有 */
    uint32_t     seq;                 /* 已发布帧计数 */
    uint32_t     shown_seq;           /* 消费端已取走的帧计数 */

    /* 拍照：请求标志由 UI 线程置、解码线程清；截下的帧放在 capture_buf 里。
     * capture_ready 为真表示有帧等着被 UI 线程取走 */
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

/* 单调时钟的秒数。用 CLOCK_MONOTONIC 而不是 time()，避免被系统改时间影响 */
static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* -------------------------------------------------------------------------
 * 等比缩放填充 + （必要时）格式转换
 *
 * 摄像头 640x360 与预览区 480x272 比例几乎一致（1.778 vs 1.765），
 * 等比缩放到铺满只需上下各留 1px，填充率 99.26%，视野零损失。
 *
 * 缩放用最近邻：这个循环本来就要逐像素跑一遍，改成缩放后**循环次数不变**，
 * 只是源索引从顺序推进变成查表（xmap 启动时算好）。双线性要多做 4 邻域加权，
 * 在 A7 上是三四倍的开销，先用便宜的。
 * 注意 640→480 是「缩小」，最近邻缩小会丢像素、边缘可能有锯齿，
 * 真觉得画质不行再换双线性。
 *
 * 两条路径：
 *   src_565 为真 —— 解码端已用 libjpeg-turbo 的 JCS_RGB565 直接产出 RGB565，
 *                   这里只做纯 16 位搬运，**零格式转换开销**
 *   否则         —— 解码出的是 RGB24，逐像素打包成 RGB565
 * ------------------------------------------------------------------------- */
static void convert_scale(const uint8_t *src, int src_w, int src_h,
                          uint8_t *dst, int dst_w, int dst_h)
{
    uint16_t *d = (uint16_t *)dst;

    /* 先铺满背景色，四周不足的边就自动是它了 */
    for (int i = 0; i < dst_w * dst_h; i++) d[i] = BACKDROP_RGB565;

    if (g.src_565) {
        /* 字节序无需处理：libjpeg 的 PACK_SHORT_565_LE 与我们小端机上
         * LVGL 的 RGB565 布局一致，都是 R 占 bit15-11 */
        for (int y = 0; y < g.disp_h; y++) {
            const int sy = (int)((int64_t)y * src_h / g.disp_h);
            const uint16_t *srow =
                (const uint16_t *)(src + (size_t)sy * src_w * 2);
            uint16_t *drow = d + (size_t)(y + g.off_y) * dst_w + g.off_x;

            for (int x = 0; x < g.disp_w; x++)
                *drow++ = srow[g.xmap[x]];
        }
        return;
    }

    /* RGB24 路径：逐像素取高 5/6/5 位打包 */
    for (int y = 0; y < g.disp_h; y++) {
        const int sy = (int)((int64_t)y * src_h / g.disp_h);
        const uint8_t *srow = src + (size_t)sy * src_w * 3;
        uint16_t *drow = d + (size_t)(y + g.off_y) * dst_w + g.off_x;

        for (int x = 0; x < g.disp_w; x++) {
            const uint8_t *s = srow + (size_t)g.xmap[x] * 3;
            *drow++ = RGB565(s[0], s[1], s[2]);
        }
    }
}

/* -------------------------------------------------------------------------
 * 解码线程
 *
 * 这是唯一会调用 libjpeg 的线程。它绝不碰任何 lv_* 函数。
 * ------------------------------------------------------------------------- */
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

        /* 拍照请求：把这一帧的原始 MJPEG 字节留下来给 UI 线程写文件。
         * 必须在这里做而不是让 UI 线程去读队列——队列是解码线程独占消费的。
         * memcpy 20KB 只要几十微秒，不影响预览节奏 */
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
        convert_scale(g.decoded, g.cam_w, g.cam_h, g.dst[w], g.out_w, g.out_h);

        const double t1 = now_sec();
        win_cost  += (t1 - t0);
        win_frames++;

        pthread_mutex_lock(&g.lock);
        g.published = w;
        g.seq++;
        pthread_mutex_unlock(&g.lock);

        g.write_idx = (w + 1) % CAM_BUF_COUNT;
        decoded++;

        /* 每秒报一次。帧率是后续所有取舍的依据——要不要降分辨率、
         * 要不要换更贵的缩放算法，都得看这个数字，不能靠猜 */
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
    /* 等比缩放到刚好放进输出区，再居中。整数运算，不引浮点。
     * 先按「铺满宽」试算高度，放不下再改成「铺满高」。 */
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

    /* 输出列 → 源列 的映射表。逐像素做除法在 A7 上太贵，启动时算一次存下来 */
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

    /* 让 MJPEG 直接吐 RGB565：格式转换在 libjpeg 内部就做完且走 NEON，
     * 我们省掉一整遍逐像素转换和 691KB 的中间缓冲。
     * 拿不到（说明链的是 IJG 原版，没有 JCS_RGB565）就退回 RGB24，
     * convert_scale 两条路径都支持 */
    g.src_565 = (decoder_set_mjpeg_rgb565(g.dec) == 0);
    printf("[预览] 解码输出: %s\n",
           g.src_565 ? "RGB565（无需格式转换）" : "RGB24（需逐像素转换）");

    g.decoded_size = decoder_output_size(g.dec);
    g.decoded = malloc(g.decoded_size);
    if (!g.decoded) {
        fprintf(stderr, "[预览] 解码缓冲分配失败（%zu 字节）\n", g.decoded_size);
        goto fail_dec;
    }

    /* 拍照缓冲：要装得下驱动可能给的最大帧（MJPEG 是变长格式，
     * 每帧实际长度都不同，只有 QUERYBUF 报出的上界是可靠的） */
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

    /* 队列每槽要能装下驱动报的最大帧（maxf，上面已取），
     * 否则 fq_push 会静默丢帧——cap_start 自己也会校验这一点 */
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
    /* xmap / captured_buf 在这里统一回收；未分配时 free(NULL) 也安全 */
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

int cam_preview_cam_width(void)  { return g.cam_w; }
int cam_preview_cam_height(void) { return g.cam_h; }

const char *cam_preview_cam_format(void)
{
    return g_inited ? pixel_format_name(g.fmt) : "未启动";
}

unsigned long cam_preview_dropped(void)
{
    return g.fq ? fq_dropped(g.fq) : 0;
}

double cam_preview_fps(void)     { return g.fps; }
double cam_preview_cost_ms(void) { return g.cost_ms; }
