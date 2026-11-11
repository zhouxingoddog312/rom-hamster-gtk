/*
 * dat.h — DAT（Logiqx XML）索引（ARCHITECTURE §5 / §8.4）
 *
 * 边界：dat.c 只解析 XML 并交给 store_* 落库；不打印、不搬文件。
 */
#pragma once

#include <glib.h>

#include "rh-ctx.h"
#include "rh-records.h"
#include "rh-err.h"

/* 解析一个 DAT 并把全部条目写入 dat_entries。*dat_id 可 NULL。 */
RhErr rh_dat_import_file(RhCtx *ctx, const char *platform, const char *path,
                         gint64 *dat_id);

/* 只解析不入库（测试用）：*entries = GPtrArray<RhDatEntry*>，
 * *name / *version 可为 NULL */
RhErr rh_dat_parse(const char *path, GPtrArray **entries,
                   gchar **name, gchar **version);

/*
 * 按哈希在已导入的 DAT 里找同平台条目；*out 由函数分配，未命中为 NULL，
 * 调用方用 rh_dat_match_free 释放。核心库不持任何全局缓存（§1.1）。
 */
RhErr rh_dat_match_rom(RhCtx *ctx, const char *platform, const char *crc32,
                       const char *md5, const char *sha1, RhDatMatch **out);
