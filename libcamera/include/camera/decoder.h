#ifndef DECODER_H_
#define DECODER_H_

#include <stddef.h>   // size_t

/* 像素格式：V4L2 采集端输出的原始帧格式，解码器统一转成 RGB24 */
typedef enum {
    PIX_FMT_YUYV,   // YUYV 4:2:2 交错打包，绝大多数 UVC 摄像头默认格式
    PIX_FMT_MJPEG,  // JPEG 压缩帧，解它要用 libjpeg
    PIX_FMT_RGB24,  // 已是 RGB24，直通复制
} pixel_format;

/* 不透明类型：内部结构只在 decoder.c 定义 */
typedef struct decoder decoder;

/* 创建解码器。默认输出 RGB24（width*height*3 字节）。
 * 若之后调用 decoder_set_mjpeg_rgb565()，MJPEG 的输出改为 RGB565
 * （width*height*2 字节）——缓冲大小一律以 decoder_output_size() 为准。
 * fmt/width/height 必须与采集端协商出的值一致
 * 成功返回解码器指针，失败返回 NULL */
decoder *decoder_create(pixel_format fmt, unsigned width, unsigned height);

/* 【相对上游 V4L2-Camera-App 的本地扩展，上游没有这个函数】
 * 让 MJPEG 解码直接输出 RGB565，省掉调用方一遍 RGB24→RGB565 的逐像素转换，
 * 也省掉 640x360 下 691KB 的中间缓冲。
 *
 * 依赖 libjpeg-turbo 的 JCS_RGB565 扩展；若链接的是 IJG 原版则返回 -1，
 * 解码器保持 RGB24 输出（用宏保护过，切回 IJG 版仍能编译）。
 * 必须在 decoder_decode() 之前调用。
 * 对 YUYV / RGB24 格式一律返回 -1——那两个分支只有 RGB24。 */
int decoder_set_mjpeg_rgb565(decoder *d);

/* 释放解码器 */
void decoder_destroy(decoder *d);

/* 将一帧 src 解码写入 dst。默认 RGB24，若开了 mjpeg_rgb565 则 MJPEG 输出 RGB565
 *   src/src_size : 输入帧数据及其字节数（YUYV 应为 width*height*2）
 *   dst/dst_size : 输出缓冲及其容量，不小于 decoder_output_size()
 * 成功返回 0，失败返回 -1（参数非法 / 数据损坏 / 缓冲不足） */
int decoder_decode(decoder *d, const void *src, size_t src_size,
                   void *dst, size_t dst_size);

/* 解码后一帧的字节数。默认 width*height*3；
 * 开了 mjpeg_rgb565 的 MJPEG 解码器则是 width*height*2。 */
size_t decoder_output_size(const decoder *d);

/* 【相对上游 V4L2-Camera-App 的本地扩展，上游没有这个函数】
 * 解码内存里的一张独立 JPEG，尺寸由文件头决定，不需要 decoder 对象。
 * 相册浏览照片用这个——照片是当初拍下的，尺寸事先并不知道，
 * 而 decoder_create 要求预先声明尺寸。
 *
 *   dst/dst_size : 输出缓冲及其容量。按 dst_size 反推容量不够就直接失败，
 *                  所以调用方可以先按「最坏情况」分配（比如宽*高*2）
 *   rgb565       : 非 0 输出 RGB565（2 字节/像素），否则 RGB24
 *   out_w/out_h  : 非 NULL 时写回实际解码尺寸
 * 成功返回 0，失败返回 -1（数据损坏 / 容量不足） */
int decoder_decode_jpeg(const void *src, size_t src_size,
                        void *dst, size_t dst_size, int rgb565,
                        unsigned *out_w, unsigned *out_h);

/* 【相对上游 V4L2-Camera-App 的本地扩展】
 * 只读 JPEG 头拿尺寸，不解码。
 * 有了它调用方才能精确分配 decoder_decode_jpeg 需要的输出缓冲，
 * 而不是按最坏情况猜大小。成功返回 0 */
int decoder_jpeg_size(const void *src, size_t src_size,
                      unsigned *out_w, unsigned *out_h);

/* 【相对上游 V4L2-Camera-App 的本地扩展】
 * 当前链接的 libjpeg 是否支持 RGB565 直出（只有 libjpeg-turbo 有）。
 *
 * 必须在调用 decoder_decode_jpeg 之前问一下：返回 0 时那个 rgb565 参数
 * 会被忽略、一律输出 RGB24，调用方若按 2 字节/像素分配缓冲，
 * 尺寸校验会失败——表现为「明明图片没问题却解不开」。 */
int decoder_have_rgb565(void);

/* 像素格式的可读名字（日志用） */
const char *pixel_format_name(pixel_format fmt);

#endif // DECODER_H_
