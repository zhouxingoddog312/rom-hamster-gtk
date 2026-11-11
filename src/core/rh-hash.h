/*
 * rh-hash.h — 文件哈希（ARCHITECTURE §8.2：一次读盘三哈希）
 */
#pragma once

#include <glib.h>

#include "rh-err.h"

typedef struct {
  gint64 size;
  gchar *crc32;      /* 8 位小写十六进制 */
  gchar *md5;
  gchar *sha1;
} RhHash;

/*
 * 单次顺序读盘，按需计算。不计算的字段留 NULL。
 * 返回 0 成功、1 失败（err 填原因）。
 */
int  rh_hash_file(const char *path, gboolean want_md5, gboolean want_sha1,
                  RhHash *out, RhErr *err);
void rh_hash_clear(RhHash *h);
