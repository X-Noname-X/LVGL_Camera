#ifndef PHOTO_VIEW_H
#define PHOTO_VIEW_H

#include <stddef.h>
#include <stdint.h>

/*
 * 相册显示用：把一张照片读进来、解码、等比缩放到不超过给定尺寸，
 * 输出可直接喂给 lv_image 的 RGB565 缓冲。
 *
 * 和摄像头预览那条链路的区别：那里的尺寸是事先协商好的，这里是打开文件
 * 才知道——所以要先读 JPEG 头拿到尺寸，才能精确分配缓冲。
 */

/* 打开一张照片。
 *   path         照片文件路径
 *   max_w/max_h  显示区域的最大尺寸，照片等比缩放到不超过它（不裁切、不留边）
 *   out_w/out_h  非 NULL 时写回缩放后的实际尺寸
 *
 * 成功返回 RGB565 缓冲指针（**内部持有，不要 free**），失败返回 NULL
 * 并把原因打到 stderr。
 *
 * 再次调用会先释放上一次的缓冲——同一时刻只有一张照片驻留内存。
 * 用完调 photo_view_close()。 */
const uint8_t *photo_view_open(const char *path, int max_w, int max_h,
                               int *out_w, int *out_h);

/* 释放驻留的照片缓冲。可重复调用 */
void photo_view_close(void);

#endif /* PHOTO_VIEW_H */
