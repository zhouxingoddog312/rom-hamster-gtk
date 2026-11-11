/*
 * rh-hash.c — 一次读盘算 crc32/md5/sha1（zlib crc32 + GChecksum）
 */
#include "rh-hash.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

int rh_hash_file(const char *path, gboolean want_md5, gboolean want_sha1,
                 RhHash *out, RhErr *err)
{
  FILE *fp;
  guchar *buf;
  uLong crc = crc32(0L, Z_NULL, 0);
  GChecksum *md5 = want_md5 ? g_checksum_new(G_CHECKSUM_MD5) : NULL;
  GChecksum *sha1 = want_sha1 ? g_checksum_new(G_CHECKSUM_SHA1) : NULL;
  gint64 total = 0;

  g_return_val_if_fail(out != NULL, 1);
  memset(out, 0, sizeof *out);
  out->size = -1;

  fp = fopen(path, "rb");
  if (!fp) {
    rh_err_set_errno(err, RH_ERR_IO, "无法读取 %s", path);
    if (md5) g_checksum_free(md5);
    if (sha1) g_checksum_free(sha1);
    return 1;
  }

  buf = g_malloc(256 * 1024);
  for (;;) {
    size_t n = fread(buf, 1, 256 * 1024, fp);

    if (n == 0) {
      if (ferror(fp)) {
        rh_err_set_errno(err, RH_ERR_IO, "读取失败 %s", path);
        g_free(buf);
        fclose(fp);
        if (md5) g_checksum_free(md5);
        if (sha1) g_checksum_free(sha1);
        return 1;
      }
      break;
    }
    crc = crc32(crc, buf, (uInt) n);
    if (md5)
      g_checksum_update(md5, buf, n);
    if (sha1)
      g_checksum_update(sha1, buf, n);
    total += (gint64) n;
  }
  g_free(buf);
  fclose(fp);

  out->size = total;
  out->crc32 = g_strdup_printf("%08lx", (unsigned long) (crc & 0xffffffffUL));
  if (md5)
    out->md5 = g_strdup(g_checksum_get_string(md5));
  if (sha1)
    out->sha1 = g_strdup(g_checksum_get_string(sha1));
  if (md5)
    g_checksum_free(md5);
  if (sha1)
    g_checksum_free(sha1);
  return 0;
}

void rh_hash_clear(RhHash *h)
{
  if (!h)
    return;
  g_clear_pointer(&h->crc32, g_free);
  g_clear_pointer(&h->md5, g_free);
  g_clear_pointer(&h->sha1, g_free);
  h->size = -1;
}
