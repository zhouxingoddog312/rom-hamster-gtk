/*
 * rh-fs.h — 文件系统工具（ARCHITECTURE §11 写入一致性）
 *
 * 约定：复制一律"临时名 → rename"；已存在且 size+mtime 一致视为同一文件，跳过。
 */
#pragma once

#include <glib.h>

#include "rh-err.h"

gboolean rh_fs_is_file(const char *path);
gboolean rh_fs_is_dir(const char *path);
gint64   rh_fs_size(const char *path);          /* -1 = 取不到 */

int  rh_fs_mkdir_p(const char *path, RhErr *err);

/*
 * 原子复制。返回：
 *   0 = 已复制
 *   2 = 目标已存在且 size+mtime 一致（幂等跳过，不覆盖）
 *   3 = 目标已存在但不同，且 overwrite = FALSE（调用方决定跳过）
 *   4 = 已取消（仅 cancellable 版本）
 *   1 = 失败（err 填原因）
 */
int rh_fs_copy_atomic(const char *src, const char *dest, gboolean overwrite,
                      RhErr *err);

/* 带协作取消的原子复制；cancel_flag 非 NULL 时按块检查。
 * 返回码同 rh_fs_copy_atomic，另加 4 = 已取消。 */
int rh_fs_copy_atomic_cancellable(const char *src, const char *dest,
                                  gboolean overwrite, gint *cancel_flag,
                                  RhErr *err);

/* 文本原子写（tmp + rename） */
int rh_fs_write_text_atomic(const char *path, const char *text, RhErr *err);

/* 递归收集普通文件（绝对值，按路径排序）；root 非目录时返回只含 root 的数组 */
GPtrArray *rh_fs_walk_files(const char *root);

/* 内容嗅探：真实图片（按魔数），不是按扩展名 */
gboolean rh_fs_is_image(const char *path);
/* 递归删除目录或文件（用于清理导入临时目录） */
int rh_fs_remove_tree(const char *path);

/* 内容嗅探：文本（无 NUL，且控制字符占比低） */
gboolean rh_fs_is_text(const char *path);
