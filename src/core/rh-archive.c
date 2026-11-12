/*
 * archive.c — libarchive 封装
 */
#include "rh-archive.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rh-fs.h"

RhArcEntry *rh_arc_entry_new(void)
{
  return g_new0(RhArcEntry, 1);
}

void rh_arc_entry_free(RhArcEntry *e)
{
  if (!e)
    return;
  g_free(e->name);
  g_free(e);
}

static struct archive *open_reader(const char *path, const char *password)
{
  struct archive *a = archive_read_new();

  archive_read_support_filter_all(a);
  archive_read_support_format_all(a);
  if (password && *password)
    archive_read_add_passphrase(a, password);
  if (archive_read_open_filename(a, path, 256 * 1024) != ARCHIVE_OK) {
    archive_read_free(a);
    return NULL;
  }
  return a;
}

int rh_arc_list(const char *archive_path, const char *password,
                GPtrArray **out, RhErr *err)
{
  struct archive *a;
  struct archive_entry *entry;
  GPtrArray *list;
  int rc = ARCHIVE_OK;

  g_return_val_if_fail(out != NULL, 1);
  *out = NULL;

  a = open_reader(archive_path, password);
  if (!a) {
    rh_err_set(err, RH_ERR_IO, "无法读取压缩包：%s", archive_path);
    return 1;
  }

  list = g_ptr_array_new_with_free_func((GDestroyNotify) rh_arc_entry_free);
  while ((rc = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
    RhArcEntry *e = rh_arc_entry_new();
    const gchar *name = archive_entry_pathname(entry);

    e->name = g_strdup(name ? name : "");
    e->size = archive_entry_size_is_set(entry) ? (gint64) archive_entry_size(entry) : -1;
    e->is_dir = archive_entry_filetype(entry) == AE_IFDIR ||
                (name && g_str_has_suffix(name, "/"));
    e->encrypted = archive_entry_is_encrypted(entry) ? TRUE : FALSE;
    g_ptr_array_add(list, e);
  }
  archive_read_free(a);

  if (rc != ARCHIVE_EOF && list->len == 0) {
    g_ptr_array_unref(list);
    rh_err_set(err, RH_ERR_IO, "压缩包条目读不完：%s", archive_path);
    return 1;
  }
  *out = list;
  return 0;
}

gboolean rh_arc_is_encrypted(const char *archive_path)
{
  struct archive *a;
  struct archive_entry *entry;
  gboolean found = FALSE;

  a = open_reader(archive_path, NULL);
  if (!a) {
    /* 头部读不出来也常因加密；交给调用方按"读不了"处理 */
    return TRUE;
  }
  while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
    if (archive_entry_is_encrypted(entry)) {
      found = TRUE;
      break;
    }
  }
  archive_read_free(a);
  return found;
}

/* 拒绝绝对路径与 ".." 分量，防路径穿越 */
static gboolean safe_relpath(const char *name, gchar **clean)
{
  gchar **parts;
  GString *acc;
  gboolean ok = TRUE;

  *clean = NULL;
  if (!name || !*name)
    return FALSE;
  if (name[0] == '/')
    return FALSE;

  parts = g_strsplit(name, "/", -1);
  acc = g_string_new(NULL);
  for (gint i = 0; parts[i]; i++) {
    if (strcmp(parts[i], "..") == 0) {
      ok = FALSE;
      break;
    }
    if (!*parts[i] || strcmp(parts[i], ".") == 0)
      continue;
    if (acc->len)
      g_string_append_c(acc, '/');
    g_string_append(acc, parts[i]);
  }
  g_strfreev(parts);

  if (!ok || acc->len == 0) {
    g_string_free(acc, TRUE);
    return FALSE;
  }
  *clean = g_string_free(acc, FALSE);
  return TRUE;
}

static int arc_extract_impl(const char *archive_path, const char *dest,
                            const char *password, gint *cancel_flag,
                            RhErr *err)
{
  struct archive *a;
  struct archive_entry *entry;
  int rc = ARCHIVE_OK;
  int ret = 1;

  if (!rh_fs_mkdir_p(dest, err))
    return 1;

  a = open_reader(archive_path, password);
  if (!a) {
    rh_err_set(err, RH_ERR_IO, "无法读取压缩包：%s", archive_path);
    return 1;
  }

  while ((rc = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
    gchar *clean = NULL;
    gchar *path = NULL;

    if (cancel_flag && g_atomic_int_get(cancel_flag)) {
      rh_err_set(err, RH_ERR_CANCELLED, "解压已取消：%s", archive_path);
      ret = 2;
      goto done;
    }
    if (!safe_relpath(archive_entry_pathname(entry), &clean))
      continue;                        /* 可疑条目直接丢弃，不解到盘上 */
    path = g_build_filename(dest, clean, NULL);
    g_free(clean);

    if (archive_entry_filetype(entry) == AE_IFDIR) {
      g_mkdir_with_parents(path, 0755);
      g_free(path);
      continue;
    }
    if (archive_entry_filetype(entry) != AE_IFREG) {
      g_free(path);                    /* 符号链接/设备不落盘 */
      continue;
    }

    {
      gchar *dir = g_path_get_dirname(path);
      g_mkdir_with_parents(dir, 0755);
      g_free(dir);
    }

    {
      int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

      if (fd < 0) {
        rh_err_set_errno(err, RH_ERR_IO, "无法写入 %s", path);
        g_free(path);
        goto done;
      }
      for (;;) {
        const void *buf;
        size_t len;
        off_t doff = 0;
        int step;

        if (cancel_flag && g_atomic_int_get(cancel_flag)) {
          rh_err_set(err, RH_ERR_CANCELLED, "解压已取消：%s", archive_path);
          close(fd);
          g_free(path);
          ret = 2;
          goto done;
        }
        step = archive_read_data_block(a, &buf, &len, &doff);
        if (step == ARCHIVE_EOF)
          break;
        if (step != ARCHIVE_OK) {
          rh_err_set(err, RH_ERR_IO, "解压数据失败：%s（%s）", path,
                     archive_error_string(a));
          close(fd);
          g_free(path);
          goto done;
        }
        for (size_t off = 0; off < len;) {
          ssize_t w = write(fd, (const char *) buf + off, len - off);

          if (w < 0) {
            if (errno == EINTR)
              continue;
            rh_err_set_errno(err, RH_ERR_IO, "写入失败 %s", path);
            close(fd);
            g_free(path);
            goto done;
          }
          off += (size_t) w;
        }
      }
      close(fd);
    }
    g_free(path);
  }
  if (rc != ARCHIVE_EOF) {
    rh_err_set(err, RH_ERR_IO, "解压中断：%s（%s）", archive_path,
               archive_error_string(a));
    goto done;
  }
  ret = 0;

done:
  archive_read_free(a);
  return ret;
}

int rh_arc_extract(const char *archive_path, const char *dest,
                   const char *password, RhErr *err)
{
  return arc_extract_impl(archive_path, dest, password, NULL, err);
}

int rh_arc_extract_cancellable(const char *archive_path, const char *dest,
                               const char *password, gint *cancel_flag,
                               RhErr *err)
{
  return arc_extract_impl(archive_path, dest, password, cancel_flag, err);
}

int rh_arc_create_zip(const char *archive_path, GPtrArray *files, RhErr *err)
{
  struct archive *a = archive_write_new();
  int ret = 1;

  archive_write_set_format_zip(a);
  if (archive_write_open_filename(a, archive_path) != ARCHIVE_OK) {
    rh_err_set(err, RH_ERR_IO, "无法创建 %s（%s）", archive_path,
               archive_error_string(a));
    archive_write_free(a);
    return 1;
  }

  for (guint i = 0; i < files->len; i++) {
    const gchar *path = files->pdata[i];
    gchar *base = g_path_get_basename(path);
    struct stat st;
    struct archive_entry *entry;
    FILE *fp;

    if (stat(path, &st) != 0) {
      rh_err_set_errno(err, RH_ERR_IO, "无法读取 %s", path);
      g_free(base);
      goto done;
    }
    entry = archive_entry_new();
    archive_entry_set_pathname(entry, base);
    archive_entry_set_size(entry, st.st_size);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_mtime(entry, st.st_mtime, 0);
    if (archive_write_header(a, entry) != ARCHIVE_OK) {
      rh_err_set(err, RH_ERR_IO, "写入条目失败：%s", base);
      archive_entry_free(entry);
      g_free(base);
      goto done;
    }
    fp = fopen(path, "rb");
    if (!fp) {
      rh_err_set_errno(err, RH_ERR_IO, "无法读取 %s", path);
      archive_entry_free(entry);
      g_free(base);
      goto done;
    }
    {
      guchar *buf = g_malloc(256 * 1024);
      size_t n;

      while ((n = fread(buf, 1, 256 * 1024, fp)) > 0)
        archive_write_data(a, buf, n);
      g_free(buf);
    }
    fclose(fp);
    archive_entry_free(entry);
    g_free(base);
  }

  archive_write_close(a);
  archive_write_free(a);
  return 0;

done:
  archive_write_close(a);
  archive_write_free(a);
  return ret;
}
