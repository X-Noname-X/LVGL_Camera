#ifndef CAMERA_PREVIEW_H
#define CAMERA_PREVIEW_H

#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uint8_t / uint32_t */

/*
 * 摄像头预览：后台采集解码成 RGB565，LVGL 主线程取最新一帧来画。
 *
 * libcamera 采集线程 → 队列 → 本模块解码线程 → 三缓冲 → LVGL 主线程
 *
 * 【硬约束】LVGL 不是线程安全的，解码线程绝不能碰任何 lv_* 函数。
 */

/* 启动预览。
 *   out_w/out_h   输出到屏幕的尺寸，必须与预览控件尺寸一致
 *   want_w/want_h 期望的摄像头分辨率，驱动可能给别的
 * 成功返回 0，失败返回 -1（细节看 stderr） */
int cam_preview_start(const char *device,
                      int out_w, int out_h,
                      int want_w, int want_h);

/* 停止采集线程并释放全部缓冲。可重复调用 */
void cam_preview_stop(void);

/* LVGL 主线程调用：取最新一帧的 RGB565 缓冲，没有新帧返回 NULL。
 * 指针指向内部三缓冲之一，只在本次渲染期间有效，不要存下来 */
const uint8_t *cam_preview_frame(uint32_t *seq);

/* 拍照
 *
 * 每个 MJPEG 帧本身就是一张完整 JPEG，直接写盘即可——零编码、零画质损失，
 * 存下来还是摄像头原始分辨率。
 *
 * 拆成「解码线程截帧」+「UI 线程写文件」：写盘要 10~50ms，放解码线程里会
 * 卡住预览；而队列是解码线程独占消费的，UI 线程读不了。 */

/* 请求拍一张。UI 线程调用，只置个标志就返回，不阻塞。
 * 真正的截帧发生在解码线程下一次取到帧时（memcpy，几十微秒） */
void cam_preview_request_capture(void);

/* 取走已截下的原始 MJPEG 帧。
 * 取到返回帧大小（字节），*data 指向内部缓冲；还没截到返回 0。
 *
 * 【缓冲所有权】取走后到下次 request_capture 生效前，这块缓冲归调用方，
 * 可以放心慢慢写；但别把指针存下来跨请求使用。 */
size_t cam_preview_take_captured(const uint8_t **data);

#endif /* CAMERA_PREVIEW_H */
