#include "photo_view.h"

#include "camera/decoder.h"   /* decoder_jpeg_size / decoder_decode_jpeg */

#include <stdio.h>
#include <stdlib.h>

/* 解码结果（照片原始尺寸的 RGB565）与缩放结果，各自缓存复用。
 * 两个缓冲都只增不减：来回翻页时尺寸通常一样，就不用反复 malloc/free。 */
static uint8_t *g_dec;
static size_t   g_dec_cap;
static uint8_t *g_out;
static size_t   g_out_cap;

static void release_all(void)
{
    free(g_dec);
    free(g_out);
    g_dec = g_out = NULL;
    g_dec_cap = g_out_cap = 0;
}

void photo_view_close(void)
{
    release_all();
}

/* 把整个文件读进内存。成功返回缓冲（调用方负责 free），大小写入 *size */
static uint8_t *read_whole_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long  n;
    uint8_t *buf;

    if (f == NULL) {
        fprintf(stderr, "[相册] 打不开 %s\n", path);
        return NULL;
    }

    /* 照片只有几十 KB，用 ftell 定位大小足够；
     * 32 位 ARM 上 long 是 32 位，超过 2GB 的文件会算错——本项目不可能遇到 */
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0) {
        fprintf(stderr, "[相册] %s 读不到长度\n", path);
        fclose(f);
        return NULL;
    }
    rewind(f);

    buf = malloc((size_t)n);
    if (buf == NULL) {
        fprintf(stderr, "[相册] 分配 %ld 字节失败\n", n);
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "[相册] %s 读取不完整\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

/* 按需扩容。只有目标比现有容量大时才 realloc，缩小时保留不释放——
 * 翻页时尺寸基本恒定，反复申请释放没意义 */
static int ensure(uint8_t **buf, size_t *cap, size_t need)
{
    if (need <= *cap) return 0;

    uint8_t *p = realloc(*buf, need);
    if (p == NULL) {
        fprintf(stderr, "[相册] 分配 %zu 字节失败\n", need);
        return -1;
    }
    *buf = p;
    *cap = need;
    return 0;
}

/* RGB888 → RGB565：各通道取高位打包。和摄像头预览用的是同一套位序 */
#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

/* 最近邻等比缩放到 RGB565。
 *
 * 输出恒为 RGB565（给 lv_image 用），输入按 src_bpp 分两种：
 *   2 —— 解码直出的 RGB565，纯 16 位搬运，零转换
 *   3 —— IJG 回退路径的 RGB24，逐像素打包
 *
 * 这里是**一次性**的（翻页时才跑），不是每帧都跑，所以逐像素做除法
 * 完全可以接受——97k 个像素在带硬件除法器的 A7 上约 1.8ms，
 * 而前面那步 libjpeg 解码就要 30ms 上下，除法的开销可以忽略。
 * 摄像头预览那条路径不同，那里每帧都要跑，所以用查表。 */
static void scale_to_rgb565(const uint8_t *src, int sw, int sh, int src_bpp,
                            uint8_t *dst, int dw, int dh)
{
    uint16_t *d = (uint16_t *)dst;

    for (int y = 0; y < dh; y++) {
        const int sy = (int)((int64_t)y * sh / dh);
        uint16_t *drow = d + (size_t)y * dw;

        if (src_bpp == 2) {
            const uint16_t *srow = (const uint16_t *)src + (size_t)sy * sw;
            for (int x = 0; x < dw; x++)
                drow[x] = srow[(int64_t)x * sw / dw];
        }
        else {
            const uint8_t *srow = src + (size_t)sy * sw * 3;
            for (int x = 0; x < dw; x++) {
                const uint8_t *p = srow + (size_t)((int64_t)x * sw / dw) * 3;
                drow[x] = RGB565(p[0], p[1], p[2]);
            }
        }
    }
}

const uint8_t *photo_view_open(const char *path, int max_w, int max_h,
                               int *out_w, int *out_h)
{
    uint8_t *file;
    size_t   fsize = 0;
    unsigned sw = 0, sh = 0;
    int      dw, dh, src_bpp;

    if (path == NULL || max_w <= 0 || max_h <= 0) return NULL;

    release_all();   /* 先放掉上一张，再谈新的 */

    /* 解码输出是 RGB565 还是 RGB24，取决于链的是哪个 libjpeg。
     * 必须问清楚再分配缓冲——按 2 字节/像素分配却拿到 RGB24，
     * 尺寸校验会失败，表现成「图片明明没问题却解不开」 */
    src_bpp = decoder_have_rgb565() ? 2 : 3;

    file = read_whole_file(path, &fsize);
    if (file == NULL) return NULL;

    /* 先读文件头拿到尺寸，才能精确分配解码缓冲。
     * 不这么做就只能按最坏情况猜——要么浪费几 MB，要么猜小了直接失败 */
    if (decoder_jpeg_size(file, fsize, &sw, &sh) != 0 || sw == 0 || sh == 0) {
        fprintf(stderr, "[相册] %s 不是有效 JPEG\n", path);
        free(file);
        return NULL;
    }

    if (ensure(&g_dec, &g_dec_cap, (size_t)sw * sh * (size_t)src_bpp) != 0) {
        free(file);
        return NULL;
    }

    /* 和摄像头预览一样尽量走 RGB565 直出：色彩转换在 libjpeg 内部完成且走 NEON，
     * 拿到就能和 LVGL 直接对接。src_bpp 为 3 时说明是 IJG 回退路径，
     * 这里会拿到 RGB24，后面缩放那步会顺手打包成 RGB565 */
    if (decoder_decode_jpeg(file, fsize, g_dec, g_dec_cap,
                            src_bpp == 2, &sw, &sh) != 0) {
        fprintf(stderr, "[相册] 解码 %s 失败\n", path);
        free(file);
        return NULL;
    }
    free(file);

    /* 等比缩放到不超过 max_w x max_h。整数运算，先按铺满宽试算高 */
    if ((int64_t)max_w * sh <= (int64_t)max_h * sw) {
        dw = max_w;
        dh = (int)((int64_t)max_w * sh / sw);
    }
    else {
        dh = max_h;
        dw = (int)((int64_t)max_h * sw / sh);
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    /* 不放大。照片比显示区小就按原尺寸出，两个理由：
     * 一是放大不会增加任何信息、只是把像素摊开，二是最近邻放大很糊。
     * 等比缩放保证 dw>sw 与 dh>sh 同时成立，所以两个都截到原尺寸即可。 */
    if (dw > (int)sw || dh > (int)sh) {
        dw = (int)sw;
        dh = (int)sh;
    }

    if (ensure(&g_out, &g_out_cap, (size_t)dw * dh * 2) != 0) return NULL;

    scale_to_rgb565(g_dec, (int)sw, (int)sh, src_bpp, g_out, dw, dh);

    if (out_w) *out_w = dw;
    if (out_h) *out_h = dh;
    return g_out;
}
