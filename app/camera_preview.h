#ifndef CAMERA_PREVIEW_H
#define CAMERA_PREVIEW_H

#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uint8_t / uint32_t */

/*
 * 摄像头预览：后台采集解码成 RGB565，LVGL 主线程取最新一帧来画。
 *
 * 线程模型（共 3 个线程）：
 *   libcamera 内部采集线程   DQBUF → fq_push
 *   本模块解码线程           fq_pop → libjpeg 解码 → 转 RGB565 → 发布
 *   LVGL 主线程              cam_preview_frame() 取指针 → 渲染
 *
 * 【硬约束】LVGL 不是线程安全的。解码线程绝不能碰任何 lv_* 函数。
 * 两边只通过 cam_preview_frame() 交换一个裸缓冲指针，由主线程去渲染。
 */

/* 启动预览。
 *   device       摄像头设备，如 "/dev/video0"
 *   out_w/out_h  输出到屏幕的尺寸，必须与预览控件尺寸一致
 *   want_w/want_h 期望的摄像头分辨率，驱动可能给别的，用下面两个函数查实际值
 * 成功返回 0，失败返回 -1（细节看 stderr） */
int cam_preview_start(const char *device,
                      int out_w, int out_h,
                      int want_w, int want_h);

/* 停止采集线程并释放全部缓冲。可重复调用 */
void cam_preview_stop(void);

/* LVGL 主线程调用：取最新一帧的 RGB565 缓冲。
 * 有新帧返回缓冲指针、并把 *seq 写成帧序号；没有新帧返回 NULL。
 *
 * 返回的指针指向内部三缓冲之一，只在本次渲染期间有效，不要存下来。
 * 三缓冲保证生产端在消费端用完之前不会绕回来覆盖它。 */
const uint8_t *cam_preview_frame(uint32_t *seq);

/* 实际协商出来的摄像头参数（cap_create 之后才有意义） */
int         cam_preview_cam_width(void);
int         cam_preview_cam_height(void);
const char *cam_preview_cam_format(void);

/* 因队列满被丢掉的帧数（累计）。持续增长说明解码跟不上采集 */
unsigned long cam_preview_dropped(void);

/* -------------------------------------------------------------------------
 * 拍照
 *
 * 本项目的摄像头输出 MJPEG，而**每个 MJPEG 帧本身就是一张完整的 JPEG 文件**。
 * 所以拍照不需要任何编码——把原始帧字节直接写成 .jpg 即可：
 * 零编码开销、零画质损失，而且存下来是摄像头原始分辨率，不受屏幕尺寸影响。
 *
 * 两步式设计的原因：写文件要 10~50ms（flash 延迟 + 文件系统开销），
 * 放在解码线程里会卡住预览，放在 UI 线程里又不能直接读队列（那是解码线程
 * 在消费的）。所以拆成「解码线程截帧」+「UI 线程写文件」。
 * ------------------------------------------------------------------------- */

/* 请求拍一张。UI 线程调用，只置个标志就返回，不阻塞。
 * 真正的截帧发生在解码线程下一次取到帧时（memcpy，几十微秒） */
void cam_preview_request_capture(void);

/* 取走已截下的原始 MJPEG 帧。
 * 取到返回帧大小（字节），*data 指向内部缓冲；还没截到返回 0。
 *
 * 【缓冲所有权】取走后，到下一次 cam_preview_request_capture() 生效之前，
 * 这块缓冲归调用方使用，解码线程不会碰它——因为新请求只能由 UI 线程发起，
 * 而 UI 线程在写完文件前不会回到事件循环。
 * 所以可以放心慢慢地写，但**别把指针存下来跨请求使用**。 */
size_t cam_preview_take_captured(const uint8_t **data);

/* 解码线程每秒刷新的统计。启动满 1 秒后才有意义。
 *   fps     : 实际完成 解码+转换+发布 的帧率
 *   cost_ms : 单帧 解码+转换 的平均耗时
 * 两者是判断瓶颈的依据：fps 明显低于 30 而 cost_ms 接近 33ms，
 * 说明就是解码跟不上，该降分辨率或换更省的算法。 */
double cam_preview_fps(void);
double cam_preview_cost_ms(void);

#endif /* CAMERA_PREVIEW_H */
