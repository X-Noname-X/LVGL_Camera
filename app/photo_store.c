#include "photo_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char g_dir[PATH_MAX];

/* 判断目录项是不是我们的照片，是则返回它的编号，否则返回 0。
 *
 * 用 %c 吃掉可能的尾巴：sscanf 是前缀匹配，直接写 "photo_%d.jpg" 的话
 * photo_0007.jpg.bak 也会算数。末尾多一个 %c，只有字符串正好在 .jpg
 * 处结束时才返回 1，多一个字符就返回 2，从而被排除。 */
static int photo_index_of(const char *name)
{
    int n;
    char extra;
    if (sscanf(name, "photo_%d.jpg%c", &n, &extra) != 1) return 0;
    return n > 0 ? n : 0;
}

/* 下一个可用编号 = 目录里最大编号 + 1。
 * 不能简单用「当前张数 + 1」——删掉中间某张后再拍会撞上已有文件 */
static int next_index(void)
{
    DIR *d = opendir(g_dir);
    if (d == NULL) return 1;   /* 目录还不存在，从 1 开始 */

    int max = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int n = photo_index_of(e->d_name);
        if (n > max) max = n;
    }
    closedir(d);
    return max + 1;
}

int photo_store_init(const char *dir)
{
    if (dir == NULL || dir[0] == '\0') return -1;

    if (strlen(dir) >= sizeof(g_dir)) {
        fprintf(stderr, "[存储] 目录路径太长: %s\n", dir);
        return -1;
    }
    snprintf(g_dir, sizeof(g_dir), "%s", dir);

    /* 已存在就什么都不做（EEXIST 不算错）；不存在才建 */
    if (mkdir(g_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[存储] 无法创建目录 %s: %s\n", g_dir, strerror(errno));
        return -1;
    }

    /* 建好了还不够，得确认真的能写——只读挂载、权限不对都会在这里暴露，
     * 而不是等到用户按下快门才失败 */
    if (access(g_dir, W_OK) != 0) {
        fprintf(stderr, "[存储] 目录不可写 %s: %s\n", g_dir, strerror(errno));
        return -1;
    }

    return 0;
}

const char *photo_store_dir(void)
{
    return g_dir;
}

int photo_store_count(void)
{
    DIR *d = opendir(g_dir);
    if (d == NULL) return 0;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (photo_index_of(e->d_name) > 0) n++;
    }
    closedir(d);
    return n;
}

int photo_store_save(const uint8_t *data, size_t size,
                     char *name_out, size_t name_cap)
{
    /* 都要留出拼接余地：g_dir 最长可占满 PATH_MAX，再拼 "/photo_0001.jpg"
     * 就越界了。tmp 还比 final 多拼一个 ".tmp"，所以必须开得更大，
     * 否则 snprintf 仍可能截断。
     * 写死 PATH_MAX 会触发 -Wformat-truncation——GCC 7 起才有这个告警，
     * 所以 ARM 用的 4.9.4 不会报，但问题真实存在 */
    char final[PATH_MAX + 64];
    char tmp[PATH_MAX + 80];
    int  idx;

    if (!data || size == 0 || g_dir[0] == '\0') return -1;

    idx = next_index();
    snprintf(final, sizeof(final), "%s/photo_%04d.jpg", g_dir, idx);
    snprintf(tmp,   sizeof(tmp),   "%s.tmp",              final);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "[存储] 无法创建 %s: %s\n", tmp, strerror(errno));
        return -1;
    }

    size_t off = 0;
    while (off < size) {
        ssize_t n = write(fd, data + off, size - off);
        if (n < 0) {
            if (errno == EINTR) continue;   /* 被信号打断，重试 */
            fprintf(stderr, "[存储] 写入失败: %s\n", strerror(errno));
            close(fd);
            unlink(tmp);
            return -1;
        }
        if (n == 0) {                       /* 常规文件不该出现，防死循环 */
            fprintf(stderr, "[存储] 写入返回 0，放弃\n");
            close(fd);
            unlink(tmp);
            return -1;
        }
        off += (size_t)n;
    }

    /* 先落盘再改名。这样断电时最多留下一个 .tmp 文件，
     * 绝不会出现「名字是 photo_0001.jpg、内容却只有半张图」的情况 */
    if (fsync(fd) != 0) {
        fprintf(stderr, "[存储] fsync 失败: %s\n", strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);

    if (rename(tmp, final) != 0) {
        fprintf(stderr, "[存储] 改名失败: %s\n", strerror(errno));
        unlink(tmp);
        return -1;
    }

    if (name_out && name_cap) snprintf(name_out, name_cap, "photo_%04d.jpg", idx);
    return 0;
}
