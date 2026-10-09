#ifndef PHOTO_VIEW_H
#define PHOTO_VIEW_H

#include <stddef.h>
#include <stdint.h>

/*
 * 相册显示用：把一张照片读进来、解码、等比缩放到不超过给定尺寸，
 * 输出可直接喂给 lv_image 的 RGB565 缓冲。
 *
 * 尺寸要打开文件才知道，所以先读 JPEG 头拿尺寸再精确分配缓冲。
 */

/* 打开一张照片。
 *   max_w/max_h  显示区域最大尺寸，等比缩放到不超过它（不裁切、不留边）
 *   out_w/out_h  非 NULL 时写回缩放后的实际尺寸
 *
 * 成功返回 RGB565 缓冲指针（**内部持有，不要 free**），失败返回 NULL。
 * 再次调用会先释放上一次的缓冲——同一时刻只有一张照片驻留内存。 */
const uint8_t *photo_view_open(const char *path, int max_w, int max_h,
                               int *out_w, int *out_h);

#endif /* PHOTO_VIEW_H */
