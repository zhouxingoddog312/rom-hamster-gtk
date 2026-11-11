/*
 * detail.c — 只读详情组装（ARCHITECTURE §5.1）
 *
 * 边界：不写库、不删文件、不拼文本。SQL 只经 store_* 接口。
 */
#include "detail.h"

#include <glib/gstdio.h>

#include "rh-config.h"
#include "rh-fs.h"
#include "store.h"

static const gchar *DETAIL_KINDS[RH_DETAIL_GROUP_COUNT] = {
  "rom", "covers", "screenshots", "videos",
  "marquees", "miximages", "intro", "patcher",
};

RhDetailFile *rh_detail_file_new(void)
{
  RhDetailFile *f = g_new0(RhDetailFile, 1);

  f->size = -1;
  return f;
}

void rh_detail_file_free(RhDetailFile *f)
{
  if (!f)
    return;
  g_free(f->filename);
  g_free(f->path);
  g_free(f);
}

const gchar *rh_detail_group_kind(int index)
{
  if (index < 0 || index >= RH_DETAIL_GROUP_COUNT)
    return NULL;
  return DETAIL_KINDS[index];
}

static void add_file(RhDetailGroup *g, const gchar *dir, const gchar *name)
{
  RhDetailFile *f = rh_detail_file_new();

  f->filename = g_strdup(name);
  f->path = g_build_filename(dir, name, NULL);
  f->exists = rh_fs_is_file(f->path);
  f->size = f->exists ? rh_fs_size(f->path) : -1;
  g_ptr_array_add(g->files, f);
}

RhErr rh_detail_load(RhCtx *ctx, const char *game_id, RhGameDetail **out)
{
  RhGameDetail *d;
  RhErr e;

  if (!ctx || !ctx->db)
    return rh_fail(RH_ERR_DB, "detail: database is not open");
  if (!game_id || !*game_id || !out)
    return rh_fail(RH_ERR_CONFIG, "detail: bad arguments");
  *out = NULL;

  d = g_new0(RhGameDetail, 1);
  for (int i = 0; i < RH_DETAIL_GROUP_COUNT; i++) {
    d->groups[i].kind = DETAIL_KINDS[i];
    d->groups[i].files = g_ptr_array_new_with_free_func(
      (GDestroyNotify) rh_detail_file_free);
  }

  e = store_game_get(ctx, game_id, &d->game);
  if (!rh_err_ok(&e))
    goto fail;

  e = store_rom_list(ctx, game_id, &d->roms);
  if (!rh_err_ok(&e))
    goto fail;

  const gchar *root = rh_config_get(ctx->config, "ROM_ROOT");
  if (!root || !*root) {
    e = rh_fail(RH_ERR_CONFIG, "detail: ROM_ROOT is not configured");
    goto fail;
  }
  d->dir = g_build_filename(root, d->game->dir_path, NULL);

  gchar *romdir = g_build_filename(d->dir, "rom", NULL);
  for (guint i = 0; i < d->roms->len; i++) {
    RhRomFile *r = g_ptr_array_index(d->roms, i);

    add_file(&d->groups[0], romdir, r->filename);
  }
  g_free(romdir);

  for (int k = 1; k < RH_DETAIL_GROUP_COUNT; k++) {
    GPtrArray *media = NULL;
    gchar *sub = g_build_filename(d->dir, DETAIL_KINDS[k], NULL);

    e = store_media_list(ctx, game_id, DETAIL_KINDS[k], &media);
    if (!rh_err_ok(&e)) {
      g_free(sub);
      goto fail;
    }
    for (guint i = 0; i < media->len; i++) {
      RhMediaFile *m = g_ptr_array_index(media, i);

      add_file(&d->groups[k], sub, m->filename);
    }
    g_ptr_array_unref(media);
    g_free(sub);
  }

  *out = d;
  return rh_ok();

fail:
  rh_detail_free(d);
  return e;
}

void rh_detail_free(RhGameDetail *d)
{
  if (!d)
    return;
  rh_game_free(d->game);
  if (d->roms)
    g_ptr_array_unref(d->roms);
  for (int i = 0; i < RH_DETAIL_GROUP_COUNT; i++) {
    if (d->groups[i].files)
      g_ptr_array_unref(d->groups[i].files);
  }
  g_free(d->dir);
  g_free(d);
}
