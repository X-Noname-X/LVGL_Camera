#ifndef PHOTO_STORE_H
#define PHOTO_STORE_H

#include <stddef.h>
#include <stdint.h>

/*
 * 照片存取。
 *
 * 存的是摄像头原始 MJPEG 帧——那本身就是一张完整 JPEG 文件，直接写盘即可，
 * 不需要任何编码，也没有画质损失。
 */

/* 指定照片目录并准备它（不存在则创建）。
 * 启动时调一次，之后 photo_store_save() 才有意义。
 * 成功返回 0，失败返回 -1（目录建不出来或不可写） */
int photo_store_init(const char *dir);

/* 当前照片目录的路径（日志/UI 用）。未初始化时返回空串 */
const char *photo_store_dir(void);

/* 把一段 JPEG 字节存成新文件。
 *
 * 文件名格式 photo_%04d.jpg，编号取**目录里已有文件的最大编号 + 1**，
 * 而不是简单递增——否则删掉中间某张之后再拍，就会覆盖已有文件。
 *
 * 写入采用「先写 .tmp、fsync、再 rename」：断电时最多留下一个 .tmp，
 * 不会出现「名字是 photo_0001.jpg 但内容只有半张图」的情况。
 *
 * 成功返回 0，文件名（不含路径）写入 name_out；失败返回 -1 */
int photo_store_save(const uint8_t *data, size_t size,
                     char *name_out, size_t name_cap);

/* 目录里现有的照片张数。目录不可读时返回 0 */
int photo_store_count(void);

/* 照片文件名的最大长度（含结尾 NUL）。"photo_99999.jpg" 才 16 字节，留足余量 */
#define PHOTO_NAME_MAX 32

/* 列出目录里的照片文件名，按编号**升序**。
 *
 * names 是长度为 max 的二维数组：char names[N][PHOTO_NAME_MAX]，
 * 调用方按 char (*)[PHOTO_NAME_MAX] 传即可。
 *
 * 返回实际写入的个数，最多 max 个——超出部分会被**静默丢弃**，
 * 需要知道有没有被截断就自己跟 photo_store_count() 比一下。
 *
 * 排序按编号数值而不是字符串：`next_index()` 用的是 %d，理论上能出现
 * 位数不同的名字（photo_9999 和 photo_10000），那种情况下字典序是错的。 */
int photo_store_list(char (*names)[PHOTO_NAME_MAX], int max);

/* 把文件名拼成完整路径（含目录）。out 至少给 PATH_MAX 级别的大小 */
void photo_store_path(const char *name, char *out, size_t cap);

/* 删掉一张照片。name 是文件名（不含目录）。
 * 成功返回 0，文件不存在或删不掉返回 -1（已落盘的原因打到 stderr）。
 *
 * 只做 unlink，不碰目录里其他东西——包括可能残留的 .tmp。
 * 那个是写盘中途断电留下的，跟这次删除无关，留着让用户自己判断 */
int photo_store_delete(const char *name);

#endif /* PHOTO_STORE_H */
