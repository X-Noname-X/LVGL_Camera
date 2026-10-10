#include "photo_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>   /* malloc / free / qsort */
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char g_dir[PATH_MAX];

/* 判断目录项是不是我们的照片，是则返回编号，否则返回 0。
 *
 * 末尾的 %c 是必需的：sscanf 是前缀匹配，写 "photo_%d.jpg" 会把
 * photo_0007.jpg.bak 也算进来，多一个 %c 才能只在 .jpg 处正好结束 */
static int photo_index_of(const char *name)
{
    int n;
    char extra;
    if (sscanf(name, "photo_%d.jpg%c", &n, &extra) != 1) return 0;
    return n > 0 ? n : 0;
}

/* 下一个可用编号 = 目录里最大编号 + 1。
 * 不能用「当前张数 + 1」——删掉中间某张后再拍会撞上已有文件 */
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
    snprintf(g_dir, sizeof(g_dir), "%s", dir);  // g_dir 赋值

    /* 已存在就什么都不做（EEXIST 不算错）；不存在才建 */
    if (mkdir(g_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[存储] 无法创建目录 %s: %s\n", g_dir, strerror(errno));
        return -1;
    }

    /* 还得确认真能写——只读挂载、权限不对在这里就暴露，而不是等按快门 */
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

void photo_store_path(const char *name, char *out, size_t cap)
{
    if (!name || !out || cap == 0) return;
    snprintf(out, cap, "%s/%s", g_dir, name);
}

int photo_store_delete(const char *name)
{
    /* 比 PATH_MAX 多留一截给 "/文件名"，否则长目录名会被截断 */
    char path[PATH_MAX + 64];

    if (!name || name[0] == '\0' || g_dir[0] == '\0') return -1;

    snprintf(path, sizeof(path), "%s/%s", g_dir, name);

    if (unlink(path) != 0) {
        fprintf(stderr, "[存储] 删除 %s 失败: %s\n", path, strerror(errno));
        return -1;
    }
    printf("[存储] 已删除 %s\n", path);
    fflush(stdout);
    return 0;
}

/* 排序用：把编号和名字绑在一起 */
typedef struct {
    int  idx;
    char name[PHOTO_NAME_MAX];
} sortable;

static int cmp_by_idx(const void *a, const void *b)
{
    int ia = ((const sortable *)a)->idx;
    int ib = ((const sortable *)b)->idx;
    return (ia > ib) - (ia < ib);
}

int photo_store_list(char (*names)[PHOTO_NAME_MAX], int max)
{
    if (!names || max <= 0 || g_dir[0] == '\0') return 0;

    DIR *d = opendir(g_dir);
    if (d == NULL) return 0;

    sortable *v = malloc(sizeof(*v) * (size_t)max);
    if (v == NULL) {
        fprintf(stderr, "[存储] 列表缓冲分配失败（%d 项）\n", max);
        closedir(d);
        return 0;
    }

    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d)) != NULL) {
        int idx = photo_index_of(e->d_name);
        if (idx <= 0) continue;               /* 不是我们的照片，跳过 */

        /* d_name 最长 255 字节而 name 只有 32。用 memcpy 而非 snprintf：
         * 长度已确认，也不会有格式串告警 */
        size_t len = strlen(e->d_name);
        if (len >= PHOTO_NAME_MAX) continue;

        v[n].idx = idx;
        memcpy(v[n].name, e->d_name, len + 1);
        n++;
    }
    closedir(d);

    /* 按数值排序，不用 strcmp——位数不同时 photo_10000 会排到 photo_9999 前 */
    qsort(v, (size_t)n, sizeof(*v), cmp_by_idx);

    for (int i = 0; i < n; i++)
        snprintf(names[i], PHOTO_NAME_MAX, "%s", v[i].name);

    free(v);
    return n;
}

int photo_store_save(const uint8_t *data, size_t size,
                     char *name_out, size_t name_cap)
{
    /* 都要留出拼接余地：g_dir 最长可占满 PATH_MAX，再拼文件名就越界。
     * tmp 比 final 还多一个 ".tmp"，所以开得更大 */
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

    /* 先写文件再改名：断电时最多留一个 .tmp，不会出现半个文件顶着正式名字 */
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
