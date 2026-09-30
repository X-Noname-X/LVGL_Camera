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

#endif /* PHOTO_STORE_H */
