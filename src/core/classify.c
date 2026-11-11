/*
 * classify.c — 单文件分类实现
 */
#include "classify.h"

#include <string.h>

#include "rh-fs.h"
#include "rh-str.h"

RhClass *rh_class_new(void)
{
  RhClass *c = g_new0(RhClass, 1);

  c->candidates = g_ptr_array_new_with_free_func(g_free);
  return c;
}

void rh_class_free(RhClass *c)
{
  if (!c)
    return;
  g_free(c->ext);
  g_free(c->platform);
  g_free(c->media_kind);
  if (c->candidates)
    g_ptr_array_unref(c->candidates);
  g_free(c);
}

static gboolean list_has(GPtrArray *arr, const char *ext)
{
  if (!arr || !ext)
    return FALSE;
  for (guint i = 0; i < arr->len; i++) {
    if (g_ascii_strcasecmp((const gchar *) arr->pdata[i], ext) == 0)
      return TRUE;
  }
  return FALSE;
}

gchar *rh_rom_extension(RhPlatformRules *rules, const char *filename)
{
  gchar *ext = platform_ext_of(filename);

  if (!ext || !*ext)
    return ext;

  /* 复合扩展名：covers 的 .png 叠在平台扩展（如 pico-8 的 .p8）之上 */
  if (g_ascii_strcasecmp(ext, ".png") == 0 ||
      g_ascii_strcasecmp(ext, ".jpg") == 0 ||
      g_ascii_strcasecmp(ext, ".jpeg") == 0 ||
      g_ascii_strcasecmp(ext, ".webp") == 0) {
    gchar *stem = rh_base_noext(filename);
    gchar *inner = platform_ext_of(stem);
    GPtrArray *plats = inner ? platform_by_extension(rules, inner) : NULL;

    if (plats && plats->len > 0) {
      gchar *joined = g_strconcat(inner, ext, NULL);

      g_free(stem);
      g_free(inner);
      g_free(ext);
      return joined;
    }
    g_free(stem);
    g_free(inner);
  }
  return ext;
}

const gchar *rh_dual_role_kind(const char *path, const char *ext)
{
  if (!ext)
    return "rom";
  if (g_ascii_strcasecmp(ext, ".jpg") == 0 ||
      g_ascii_strcasecmp(ext, ".jpeg") == 0 ||
      g_ascii_strcasecmp(ext, ".png") == 0 ||
      g_ascii_strcasecmp(ext, ".webp") == 0)
    return rh_fs_is_image(path) ? "media" : "rom";
  if (g_ascii_strcasecmp(ext, ".md") == 0 ||
      g_ascii_strcasecmp(ext, ".txt") == 0)
    return rh_fs_is_text(path) ? "media" : "rom";
  return "rom";
}

int rh_classify_file(RhPlatformRules *rules, const char *path,
                     RhClass *out, RhErr *err)
{
  gchar *ext;
  GPtrArray *plats;
  GPtrArray *all;

  g_return_val_if_fail(out != NULL, 1);
  out->kind = RH_CLASS_UNKNOWN;
  g_clear_pointer(&out->ext, g_free);
  g_clear_pointer(&out->platform, g_free);
  g_clear_pointer(&out->media_kind, g_free);
  g_ptr_array_set_size(out->candidates, 0);

  if (!rules || !path) {
    rh_err_set(err, RH_ERR_CONFIG, "分类缺少平台规则或路径");
    return 1;
  }

  ext = rh_rom_extension(rules, path);
  out->ext = g_strdup(ext);
  if (!ext || !*ext) {
    g_free(ext);
    rh_err_set(err, RH_ERR_UNSUPPORTED, "无扩展名：%s", path);
    return 1;
  }

  /* 1. 压缩包 */
  if (platform_is_archive(rules, ext)) {
    out->kind = RH_CLASS_ARCHIVE;
    g_free(ext);
    return 0;
  }

  /* 2. 通用补丁（patcher_generic） */
  {
    const gchar *mk = platform_media_kind(rules, path);

    if (mk && strcmp(mk, RH_MEDIA_KIND_PATCHER) == 0) {
      out->kind = RH_CLASS_PATCH_GENERIC;
      g_free(ext);
      return 0;
    }
  }

  /* 3. 平台二义后缀 */
  all = platform_all(rules);
  for (guint i = 0; i < all->len; i++) {
    RhPlatform *p = all->pdata[i];

    if (list_has(p->ambiguous_formats, ext)) {
      out->kind = RH_CLASS_AMBIGUOUS;
      out->platform = g_strdup(p->id);
      g_free(ext);
      return 0;
    }
  }

  /* 4. 平台专属补丁 */
  for (guint i = 0; i < all->len; i++) {
    RhPlatform *p = all->pdata[i];

    if (list_has(p->patch_formats, ext)) {
      out->kind = RH_CLASS_PATCH_PLATFORM;
      out->platform = g_strdup(p->id);
      g_free(ext);
      return 0;
    }
  }

  /* 5. ROM 本体（重名扩展名先判内容） */
  plats = platform_by_extension(rules, ext);
  if (plats && plats->len > 0) {
    const gchar *mk = platform_media_kind(rules, path);

    if (mk && strcmp(rh_dual_role_kind(path, ext), "media") == 0) {
      /* 落给下面的媒体分支 */
    } else {
      out->kind = RH_CLASS_ROM;
      for (guint i = 0; i < plats->len; i++) {
        RhPlatform *p = plats->pdata[i];

        g_ptr_array_add(out->candidates, g_strdup(p->id));
      }
      if (plats->len == 1)
        out->platform = g_strdup(((RhPlatform *) plats->pdata[0])->id);
      g_free(ext);
      return 0;
    }
  }

  /* 6. 媒体 / 说明 */
  {
    const gchar *mk = platform_media_kind(rules, path);

    if (mk) {
      out->kind = RH_CLASS_MEDIA;
      out->media_kind = g_strdup(mk);
      g_free(ext);
      return 0;
    }
  }

  g_free(ext);
  rh_err_set(err, RH_ERR_UNSUPPORTED, "未识别的文件类型：%s", path);
  return 1;
}
