/*
 * rh-fs.c — 文件系统工具实现（Linux 桌面）
 */
#define _GNU_SOURCE
#include "rh-fs.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

gboolean rh_fs_is_file(const char *path)
{
  struct stat st;

  return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

gboolean rh_fs_is_dir(const char *path)
{
  struct stat st;

  return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

gint64 rh_fs_size(const char *path)
{
  struct stat st;

  if (!path || stat(path, &st) != 0)
    return -1;
  return (gint64) st.st_size;
}

int rh_fs_mkdir_p(const char *path, RhErr *err)
{
  if (!path || !*path) {
    rh_err_set(err, RH_ERR_CONFIG, "空目录路径");
    return 0;
  }
  if (g_mkdir_with_parents(path, 0755) != 0) {
    rh_err_set_errno(err, RH_ERR_IO, "无法创建目录 %s", path);
    return 0;
  }
  return 1;
}

static gboolean same_file(const char *a, const char *b)
{
  struct stat sa, sb;

  if (stat(a, &sa) != 0 || stat(b, &sb) != 0)
    return FALSE;
  return sa.st_size == sb.st_size && sa.st_mtime == sb.st_mtime;
}

static int copy_atomic_impl(const char *src, const char *dest,
                            gboolean overwrite, gint *cancel_flag, RhErr *err)
{
  gchar *destdir = NULL, *tmpl = NULL;
  int in = -1, out = -1, rc = 1;
  char buf[256 * 1024];
  struct stat sst;

  gboolean have_mode = FALSE;

  if (rh_fs_is_dir(dest)) {
    rh_err_set(err, RH_ERR_IO, "目标是一个目录：%s", dest);
    return 1;
  }
  if (g_file_test(dest, G_FILE_TEST_EXISTS)) {
    if (same_file(src, dest))
      return 2;
    if (!overwrite)
      return 3;
  }

  destdir = g_path_get_dirname(dest);
  if (!rh_fs_mkdir_p(destdir, err)) {
    g_free(destdir);
    return 1;
  }
  tmpl = g_build_filename(destdir, ".part.XXXXXX", NULL);
  out = g_mkstemp(tmpl);
  if (out < 0) {
    rh_err_set_errno(err, RH_ERR_IO, "无法创建临时文件 %s", tmpl);
    goto done;
  }
  in = open(src, O_RDONLY);
  if (in < 0) {
    rh_err_set_errno(err, RH_ERR_IO, "无法读取 %s", src);
    goto done;
  }

  for (;;) {
    ssize_t n;

    if (cancel_flag && g_atomic_int_get(cancel_flag)) {
      rh_err_set(err, RH_ERR_CANCELLED, "复制已取消：%s", src);
      rc = 4;
      goto done;
    }
    n = read(in, buf, sizeof buf);

    if (n < 0) {
      if (errno == EINTR)
        continue;
      rh_err_set_errno(err, RH_ERR_IO, "读取失败 %s", src);
      goto done;
    }
    if (n == 0)
      break;
    for (ssize_t off = 0; off < n;) {
      ssize_t w = write(out, buf + off, (size_t) (n - off));

      if (w < 0) {
        if (errno == EINTR)
          continue;
        rh_err_set_errno(err, RH_ERR_IO, "写入失败 %s", tmpl);
        goto done;
      }
      off += w;
    }
  }

  if (fstat(in, &sst) == 0) {
    struct timespec times[2];

    times[0] = sst.st_atim;
    times[1] = sst.st_mtim;
    (void) futimens(out, times);
    have_mode = TRUE;
  }

  if (close(out) != 0) {
    out = -1;
    rh_err_set_errno(err, RH_ERR_IO, "关闭临时文件失败 %s", tmpl);
    goto done;
  }
  out = -1;

  if (g_rename(tmpl, dest) != 0) {
    rh_err_set_errno(err, RH_ERR_IO, "重命名到 %s 失败", dest);
    goto done;
  }
  g_chmod(dest, have_mode ? (mode_t) (sst.st_mode & 0777) : 0644);
  rc = 0;

done:
  if (in >= 0)
    close(in);
  if (out >= 0)
    close(out);
  if (rc != 0 && tmpl)
    g_unlink(tmpl);
  g_free(tmpl);
  g_free(destdir);
  return rc;
}

int rh_fs_copy_atomic(const char *src, const char *dest, gboolean overwrite,
                      RhErr *err)
{
  return copy_atomic_impl(src, dest, overwrite, NULL, err);
}

int rh_fs_copy_atomic_cancellable(const char *src, const char *dest,
                                  gboolean overwrite, gint *cancel_flag,
                                  RhErr *err)
{
  return copy_atomic_impl(src, dest, overwrite, cancel_flag, err);
}

int rh_fs_write_text_atomic(const char *path, const char *text, RhErr *err)
{
  gchar *dir = g_path_get_dirname(path);
  gchar *tmpl = g_build_filename(dir, ".part.XXXXXX", NULL);
  int fd = -1;
  int rc = 1;
  gsize len = strlen(text), off = 0;

  if (!rh_fs_mkdir_p(dir, err))
    goto done;
  fd = g_mkstemp(tmpl);
  if (fd < 0) {
    rh_err_set_errno(err, RH_ERR_IO, "无法创建临时文件 %s", tmpl);
    goto done;
  }
  while (off < len) {
    ssize_t w = write(fd, text + off, len - off);

    if (w < 0) {
      if (errno == EINTR)
        continue;
      rh_err_set_errno(err, RH_ERR_IO, "写入失败 %s", tmpl);
      goto done;
    }
    off += (gsize) w;
  }
  close(fd);
  fd = -1;
  if (g_rename(tmpl, path) != 0) {
    rh_err_set_errno(err, RH_ERR_IO, "重命名到 %s 失败", path);
    goto done;
  }
  rc = 0;

done:
  if (fd >= 0)
    close(fd);
  if (rc != 0 && tmpl)
    g_unlink(tmpl);
  g_free(tmpl);
  g_free(dir);
  return rc;
}

static void walk_into(const char *dir, GPtrArray *out)
{
  GDir *d = g_dir_open(dir, 0, NULL);
  const gchar *name;

  if (!d)
    return;
  while ((name = g_dir_read_name(d))) {
    gchar *path = g_build_filename(dir, name, NULL);

    if (rh_fs_is_dir(path))
      walk_into(path, out);
    else if (rh_fs_is_file(path))
      g_ptr_array_add(out, path);        /* 所有权转移给数组 */
    else
      g_free(path);
  }
  g_dir_close(d);
}

static gint cmp_str(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const gchar * const *) a, *(const gchar * const *) b);
}

GPtrArray *rh_fs_walk_files(const char *root)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);

  if (!root)
    return out;
  if (rh_fs_is_dir(root))
    walk_into(root, out);
  else if (rh_fs_is_file(root))
    g_ptr_array_add(out, g_strdup(root));
  g_ptr_array_sort(out, cmp_str);
  return out;
}

static int remove_tree_rec(const char *path)
{
  GDir *d;

  if (rh_fs_is_dir(path)) {
    d = g_dir_open(path, 0, NULL);
    if (d) {
      const gchar *n;

      while ((n = g_dir_read_name(d))) {
        gchar *child = g_build_filename(path, n, NULL);

        remove_tree_rec(child);
        g_free(child);
      }
      g_dir_close(d);
    }
    return g_rmdir(path) == 0 ? 0 : 1;
  }
  return g_unlink(path) == 0 ? 0 : 1;
}

int rh_fs_remove_tree(const char *path)
{
  if (!path || !g_file_test(path, G_FILE_TEST_EXISTS))
    return 0;
  return remove_tree_rec(path);
}

gboolean rh_fs_is_image(const char *path)
{
  guchar head[12];
  FILE *fp = fopen(path, "rb");
  size_t n;

  if (!fp)
    return FALSE;
  n = fread(head, 1, sizeof head, fp);
  fclose(fp);
  if (n >= 8 && memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0)
    return TRUE;
  if (n >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF)
    return TRUE;
  if (n >= 12 && memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WEBP", 4) == 0)
    return TRUE;
  if (n >= 6 && (memcmp(head, "GIF87a", 6) == 0 || memcmp(head, "GIF89a", 6) == 0))
    return TRUE;
  return FALSE;
}

gboolean rh_fs_is_text(const char *path)
{
  guchar buf[4096];
  FILE *fp = fopen(path, "rb");
  size_t n, i, bad = 0;

  if (!fp)
    return FALSE;
  n = fread(buf, 1, sizeof buf, fp);
  fclose(fp);
  if (n == 0)
    return TRUE;
  for (i = 0; i < n; i++) {
    if (buf[i] == 0)
      return FALSE;
    if (buf[i] < 0x20 && buf[i] != '\n' && buf[i] != '\r' && buf[i] != '\t')
      bad++;
  }
  /* UTF-8 高位字节不算坏字符；低比例控制字符视为文本 */
  return bad * 20 < n;
}
