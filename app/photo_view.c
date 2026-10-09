#include "photo_view.h"

#include "camera/decoder.h"   /* decoder_jpeg_size / decoder_decode_jpeg */

#include <stdio.h>
#include <stdlib.h>

/* 解码结果与缩放结果，各自缓存复用。只增不减——翻页时尺寸通常一样 */
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

    /* 照片只有几十 KB，ftell 足够 */
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

/* 按需扩容：只在不够大时 realloc，缩小不释放 */
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

/* 最近邻等比缩放到 RGB565（解码端已直出，这里只做 16 位搬运）。
 *
 * 只在翻页时跑一次，逐像素做除法可以接受；预览那条路径每帧都要跑，所以用查表 */
static void scale_to_rgb565(const uint8_t *src, int sw, int sh,
                            uint8_t *dst, int dw, int dh)
{
    uint16_t *d = (uint16_t *)dst;

    for (int y = 0; y < dh; y++) {
        const int sy = (int)((int64_t)y * sh / dh);
        const uint16_t *srow = (const uint16_t *)src + (size_t)sy * sw;
        uint16_t *drow = d + (size_t)y * dw;

        for (int x = 0; x < dw; x++)
            drow[x] = srow[(int64_t)x * sw / dw];
    }
}

const uint8_t *photo_view_open(const char *path, int max_w, int max_h,
                               int *out_w, int *out_h)
{
    uint8_t *file;
    size_t   fsize = 0;
    unsigned sw = 0, sh = 0;
    int      dw, dh;

    if (path == NULL || max_w <= 0 || max_h <= 0) return NULL;

    release_all();   /* 先放掉上一张，再谈新的 */

    file = read_whole_file(path, &fsize);
    if (file == NULL) return NULL;

    /* 先读文件头拿尺寸，才能精确分配解码缓冲 */
    if (decoder_jpeg_size(file, fsize, &sw, &sh) != 0 || sw == 0 || sh == 0) {
        fprintf(stderr, "[相册] %s 不是有效 JPEG\n", path);
        free(file);
        return NULL;
    }

    if (ensure(&g_dec, &g_dec_cap, (size_t)sw * sh * 2) != 0) {
        free(file);
        return NULL;
    }

    if (decoder_decode_jpeg(file, fsize, g_dec, g_dec_cap, &sw, &sh) != 0) {
        fprintf(stderr, "[相册] 解码 %s 失败\n", path);
        free(file);
        return NULL;
    }
    free(file);

    /* 等比缩放到不超过 max_w x max_h，先按铺满宽试算高 */
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

    /* 不放大：放大不增加信息，最近邻放大还很糊。
     * 等比缩放保证 dw>sw 与 dh>sh 同时成立，两个都截到原尺寸即可 */
    if (dw > (int)sw || dh > (int)sh) {
        dw = (int)sw;
        dh = (int)sh;
    }

    if (ensure(&g_out, &g_out_cap, (size_t)dw * dh * 2) != 0) return NULL;

    scale_to_rgb565(g_dec, (int)sw, (int)sh, g_out, dw, dh);

    if (out_w) *out_w = dw;
    if (out_h) *out_h = dh;
    return g_out;
}
