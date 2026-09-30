#include "camera/decoder.h"

#include <stdlib.h>   // malloc / free
#include <string.h>   // memcpy
#include <stdint.h>   // uint8_t

/* MJPEG 依赖 libjpeg 的 API（jpeglib.h / jpeg_*），发行版装的几乎都是
 * libjpeg-turbo，它与 IJG 原版在 API/ABI 上兼容，所以找到哪个都行
 * 注意别跟 TurboJPEG API（turbojpeg.h）混为一谈：那是另一套高层封装 */
#include <stdio.h>    // jpeglib.h 用到 FILE，必须先包含
#include <jpeglib.h>
#include <setjmp.h>

struct decoder {
    pixel_format fmt;
    unsigned width;
    unsigned height;
    /* MJPEG 专用：置位后解码直接输出 RGB565 而非 RGB24。
     * 这是相对上游 V4L2-Camera-App 的本地扩展，见 decoder_set_mjpeg_rgb565() */
    int mjpeg_565;
};

/* 判断链的是不是 libjpeg-turbo。
 *
 * 【别改成 #ifdef JCS_RGB565】JCS_RGB565 是 J_COLOR_SPACE 枚举里的一个
 * 枚举常量，不是宏。而 #ifdef 只能检测宏，写成那样会恒为假——
 * 结果就是明明链着 turbo 却一直走 RGB24 路径，且没有任何编译期提示。
 * 踩过这个坑。
 *
 * LIBJPEG_TURBO_VERSION 由 turbo 的 jconfig.h 用 #define 给出（如 "3.0.4"），
 * IJG 原版没有，是可靠的区分标志。 */
#ifdef LIBJPEG_TURBO_VERSION
#  define HAVE_JCS_RGB565 1
#else
#  define HAVE_JCS_RGB565 0
#endif

/* 每个输出像素的字节数。MJPEG 走了 RGB565 就是 2，其余一律 3（RGB24） */
static size_t out_bpp(const decoder *d)
{
    return (d->fmt == PIX_FMT_MJPEG && d->mjpeg_565) ? 2 : 3;
}

const char *pixel_format_name(pixel_format fmt)
{
    switch (fmt) {
    case PIX_FMT_YUYV:  return "YUYV";
    case PIX_FMT_MJPEG: return "MJPEG";
    case PIX_FMT_RGB24: return "RGB24";
    default:            return "unknown";
    }
}

decoder *decoder_create(pixel_format fmt, unsigned width, unsigned height)
{
    decoder *d;

    if (width == 0 || height == 0) return NULL;

    d = malloc(sizeof(*d));
    if (!d) return NULL;
    d->fmt = fmt;
    d->width = width;
    d->height = height;
    d->mjpeg_565 = 0;
    return d;
}

int decoder_set_mjpeg_rgb565(decoder *d)
{
    if (!d) return -1;
    /* 只有 MJPEG 有这条路径；YUYV / RGB24 那两个分支固定输出 RGB24 */
    if (d->fmt != PIX_FMT_MJPEG) return -1;
#if !HAVE_JCS_RGB565
    return -1;   /* 当前链的是 IJG 原版，没有 RGB565 输出 */
#else
    d->mjpeg_565 = 1;
    return 0;
#endif
}

void decoder_destroy(decoder *d)
{
    free(d);
}

size_t decoder_output_size(const decoder *d)
{
    return d ? (size_t)d->width * d->height * out_bpp(d) : 0;
}

// 把整数截回 [0,255]（BT.601 定点运算会溢出）
static int clamp_byte(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return v;
}

/* YUYV → RGB24，BT.601 全范围整数变换，避免浮点
 * YUYV 每 4 字节 = 相邻 2 像素（Y0 U Y1 V），要求 width 为偶数（V4L2 下恒成立） */
static int yuyv_to_rgb24(const uint8_t *src, size_t src_size, unsigned width,
                         unsigned height, uint8_t *dst, size_t dst_size)
{
    size_t npix = (size_t)width * height;

    if (src_size < npix * 2) return -1;   // YUYV 每像素 2 字节
    if (dst_size < npix * 3) return -1;

    for (size_t i = 0; i < npix; i += 2) {
        int y0 = src[0], u = src[1], y1 = src[2], v = src[3];
        int d = u - 128, e = v - 128, c;
        src += 4;

        c = y0 - 16;
        *dst++ = (uint8_t)clamp_byte((298 * c + 409 * e + 128) >> 8);
        *dst++ = (uint8_t)clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8);
        *dst++ = (uint8_t)clamp_byte((298 * c + 516 * d + 128) >> 8);

        c = y1 - 16;
        *dst++ = (uint8_t)clamp_byte((298 * c + 409 * e + 128) >> 8);
        *dst++ = (uint8_t)clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8);
        *dst++ = (uint8_t)clamp_byte((298 * c + 516 * d + 128) >> 8);
    }
    return 0;
}

/* libjpeg 默认出错直接 exit()，对库是致命的，换成 setjmp 兜底返回错误码 */
struct jerr_mgr {
    struct jpeg_error_mgr pub;   // 必须是第一个成员
    jmp_buf jmp;
};

static void jerr_exit(j_common_ptr cinfo)
{
    struct jerr_mgr *e = (struct jerr_mgr *)cinfo->err;
    (*cinfo->err->output_message)(cinfo);
    longjmp(e->jmp, 1);          // 跳回 setjmp 处
}

/* MJPEG → RGB24 或 RGB565，内存解码，不落盘。
 *
 * rgb565 为真时用 libjpeg-turbo 的 JCS_RGB565 扩展，直接出 2 字节/像素。
 * 好处是色彩转换那一步在库内部就完成了、且走 NEON，
 * 调用方省掉一整遍 RGB24→RGB565 的逐像素转换，也省掉 691KB 的中间缓冲。
 * 字节序无需担心：库里的 PACK_SHORT_565_LE 与我们小端机上 LVGL 的
 * RGB565 布局一致（都是 R 占 bit15-11）。 */
static int mjpeg_to_rgb(const uint8_t *src, size_t src_size, unsigned width,
                        unsigned height, uint8_t *dst, size_t dst_size,
                        int rgb565)
{
    struct jpeg_decompress_struct cinfo;
    struct jerr_mgr jerr;
    uint8_t *row = dst;
#if !HAVE_JCS_RGB565
    rgb565 = 0;   /* 没有 turbo 扩展就退化到 RGB24，保证 bpp 与实际输出一致 */
#endif
    const int bpp = rgb565 ? 2 : 3;
    /* volatile：这两个变量在 setjmp 之后被改过，longjmp 跳回来时
     * 只有 volatile 才能保证值不被优化掉（见 C11 7.13.2.1） */
    volatile int created = 0, rc = -1;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jerr_exit;
    if (setjmp(jerr.jmp)) goto out;   // 解压中出错，跳到清理

    jpeg_create_decompress(&cinfo);
    created = 1;
    jpeg_mem_src(&cinfo, (unsigned char *)src, (unsigned long)src_size);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) goto out;

    // 分辨率必须与采集端协商的一致，否则直接判失败
    if (cinfo.image_width != width || cinfo.image_height != height) goto out;
    if (dst_size < (size_t)width * height * bpp) goto out;

#if HAVE_JCS_RGB565
    cinfo.out_color_space = rgb565 ? JCS_RGB565 : JCS_RGB;

    /* RGB565 想走「合并上采样器」必须关掉 fancy upsampling——
     * 那条路径把色度上采样和色彩转换并成一遍做，是 libjpeg-turbo 的快路径
     * （条件见 jdmaster.c 的 use_merged_upsample()）。
     * 代价是色度改用箱式滤波而非平滑插值。我们反正要缩到 480x270，
     * 这点差别看不出来，换来的提速是实打实的。*/
    if (rgb565) cinfo.do_fancy_upsampling = FALSE;
#else
    cinfo.out_color_space = JCS_RGB;   /* 没有 turbo 扩展，只能 RGB24 */
#endif

    jpeg_start_decompress(&cinfo);
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *rowptr[1] = { row };
        jpeg_read_scanlines(&cinfo, rowptr, 1);
        row += cinfo.output_width * bpp;
    }
    jpeg_finish_decompress(&cinfo);
    rc = 0;

out:
    if (created) jpeg_destroy_decompress(&cinfo);
    return rc;
}

int decoder_decode(decoder *d, const void *src, size_t src_size,
                   void *dst, size_t dst_size)
{
    if (!d || !src || !dst) return -1;

    switch (d->fmt) {
    case PIX_FMT_YUYV:
        return yuyv_to_rgb24(src, src_size, d->width, d->height, dst, dst_size);
    case PIX_FMT_MJPEG:
        return mjpeg_to_rgb(src, src_size, d->width, d->height, dst, dst_size,
                            d->mjpeg_565);
    case PIX_FMT_RGB24:
        if (src_size > dst_size) return -1;
        memcpy(dst, src, src_size);
        return 0;
    default:
        return -1;
    }
}
