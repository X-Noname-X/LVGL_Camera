#ifndef PHOTO_STORE_H
#define PHOTO_STORE_H

#include <stddef.h>
#include <stdint.h>

/*
 * 照片存取。存的是摄像头原始 MJPEG 帧——本身就是完整 JPEG，直接写文件即可。
 */

/* 指定照片目录并准备它（不存在则创建）。启动时调一次。
 * 成功返回 0，失败返回 -1（建不出来或不可写） */
int photo_store_init(const char *dir);

/* 当前照片目录的路径（日志/UI 用）。未初始化时返回空串 */
const char *photo_store_dir(void);

/* 把一段 JPEG 字节存成新文件，文件名 photo_%04d.jpg。
 *
 * 编号取目录里最大编号 + 1，而不是简单递增——否则删掉中间某张后再拍会覆盖。
 * 写入走「.tmp + fsync + rename」，断电时最多留一个 .tmp。
 *
 * 成功返回 0，文件名（不含路径）写入 name_out；失败返回 -1 */
int photo_store_save(const uint8_t *data, size_t size,
                     char *name_out, size_t name_cap);

/* 目录里现有的照片张数。目录不可读时返回 0 */
int photo_store_count(void);

/* 照片文件名的最大长度（含结尾 NUL）。"photo_99999.jpg" 才 16 字节，留足余量 */
#define PHOTO_NAME_MAX 32

/* 列出目录里的照片文件名，按编号升序。
 *
 * names 是 char names[N][PHOTO_NAME_MAX]，调用方按 char (*)[PHOTO_NAME_MAX] 传。
 * 返回实际写入个数，最多 max 个，超出部分静默丢弃。
 *
 * 按数值排序而非字符串：编号位数不同时字典序是错的。 */
int photo_store_list(char (*names)[PHOTO_NAME_MAX], int max);

/* 把文件名拼成完整路径（含目录）。out 至少给 PATH_MAX 级别的大小 */
void photo_store_path(const char *name, char *out, size_t cap);

/* 删掉一张照片。name 是文件名（不含目录）。
 * 成功返回 0，删不掉返回 -1（原因打到 stderr）。
 *
 * 只做 unlink，不碰可能残留的 .tmp——那是断电留下的，跟这次删除无关 */
int photo_store_delete(const char *name);

#endif /* PHOTO_STORE_H */
