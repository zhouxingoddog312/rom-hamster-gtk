/*
 * archive.h — 压缩包读写（ARCHITECTURE §5：libarchive 替代 unzip/7z/unrar）
 *
 * 只做"列条目 / 判加密 / 解压"三件事；分流策略在 import.c（六级分流）。
 * 解压必须防路径穿越（绝对路径、.. 分量一律拒绝）。
 */
#pragma once

#include <glib.h>

#include "rh-err.h"

typedef struct {
  gchar   *name;       /* 包内路径（原样，未解码） */
  gint64   size;
  gboolean is_dir;
  gboolean encrypted;
} RhArcEntry;

RhArcEntry *rh_arc_entry_new(void);
void        rh_arc_entry_free(RhArcEntry *e);

/* 列出条目（含目录）；*out 由函数分配，调用方 g_ptr_array_unref */
int rh_arc_list(const char *archive_path, const char *password,
                GPtrArray **out, RhErr *err);

/* 有任一加密条目即返回 TRUE；读不了返回 FALSE */
gboolean rh_arc_is_encrypted(const char *archive_path);

/* 解压到 dest（不存在则创建）。返回 0 成功、1 失败 */
int rh_arc_extract(const char *archive_path, const char *dest,
                   const char *password, RhErr *err);

/* 打一个只存不压的 zip（导出/测试用），files 为绝对路径，包内只留 basename */
int rh_arc_create_zip(const char *archive_path, GPtrArray *files, RhErr *err);
