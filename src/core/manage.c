/*
 * manage.c — 编辑/删除/媒体/完整性（ARCHITECTURE §5.1）
 *
 * 不打印、不弹菜单；失败通过 RhErr 返回，由前端决定怎么显示。
 */
#include "manage.h"

#include <errno.h>
#include <glib/gstdio.h>

#include "rh-config.h"
#include "rh-fs.h"
#include "store.h"

static const gchar *MANAGE_KINDS[] = {
  "covers", "screenshots", "videos", "marquees", "miximages", "intro",
  "patcher",
};

static gboolean kind_ok(const char *kind)
{
  if (!kind)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS(MANAGE_KINDS); i++) {
    if (g_strcmp0(kind, MANAGE_KINDS[i]) == 0)
      return TRUE;
  }
  return FALSE;
}

static gchar *game_dir(RhCtx *ctx, const RhGame *g, RhErr *err)
{
  const gchar *root = rh_config_get(ctx->config, "ROM_ROOT");
  gchar *rootc, *dirc;

  if (!root || !*root) {
    rh_err_set(err, RH_ERR_CONFIG, "ROM_ROOT is not configured");
    return NULL;
  }
  rootc = g_canonicalize_filename(root, NULL);
  dirc = g_canonicalize_filename(g->dir_path, rootc);
  gchar *prefix = g_strconcat(rootc, G_DIR_SEPARATOR_S, NULL);
  if (!g_str_has_prefix(dirc, prefix)) {
    rh_err_set(err, RH_ERR_CONFIG, "game directory escapes ROM_ROOT: %s",
               g->dir_path);
    g_free(prefix);
    g_free(rootc);
    g_free(dirc);
    return NULL;
  }
  g_free(prefix);
  g_free(rootc);
  return dirc;
}

RhErr rh_manage_update(RhCtx *ctx, const char *game_id, const RhGamePatch *p)
{
  RhGame *g = NULL;
  RhErr e;

  if (!ctx || !ctx->db)
    return rh_fail(RH_ERR_DB, "manage: database is not open");
  if (!game_id || !p)
    return rh_fail(RH_ERR_CONFIG, "manage_update: bad arguments");

  e = store_game_get(ctx, game_id, &g);
  if (!rh_err_ok(&e))
    return e;

#define REPLACE_STR(field) \
  do { \
    if (p->set_##field) { \
      g_free(g->field); \
      g->field = p->field ? g_strdup(p->field) : NULL; \
    } \
  } while (0)

  REPLACE_STR(name);
  REPLACE_STR(game_main_name);
  REPLACE_STR(description);
  REPLACE_STR(developer);
  REPLACE_STR(publisher);
  REPLACE_STR(genre);
  REPLACE_STR(region);
  REPLACE_STR(rom_status);
  REPLACE_STR(patch_type);
#undef REPLACE_STR

  if (p->set_year)
    g->year = p->year;
  if (p->set_players)
    g->players = p->players;
  if (p->set_rating)
    g->rating = p->rating;

  e = store_game_upsert(ctx, g);
  if (rh_err_ok(&e) && p->set_tags)
    e = store_tags_set(ctx, game_id, p->tags, p->n_tags);

  rh_game_free(g);
  return e;
}

RhErr rh_manage_delete(RhCtx *ctx, const char *game_id, gboolean delete_files)
{
  RhGame *g = NULL;
  gchar *dir = NULL;
  RhErr e;

  if (!ctx || !ctx->db)
    return rh_fail(RH_ERR_DB, "manage: database is not open");
  if (!game_id)
    return rh_fail(RH_ERR_CONFIG, "manage_delete: bad arguments");

  e = store_game_get(ctx, game_id, &g);
  if (!rh_err_ok(&e))
    return e;

  if (delete_files) {
    dir = game_dir(ctx, g, &e);
    if (!dir) {
      rh_game_free(g);
      return e;
    }
  }

  e = store_game_delete(ctx, game_id);
  rh_game_free(g);
  if (!rh_err_ok(&e)) {
    g_free(dir);
    return e;
  }

  if (delete_files && dir && rh_fs_is_dir(dir)) {
    if (rh_fs_remove_tree(dir) != 0) {
      e = rh_fail(RH_ERR_IO, "failed to remove directory: %s", dir);
      g_free(dir);
      return e;
    }
  }
  g_free(dir);
  return rh_ok();
}

RhErr rh_manage_add_media(RhCtx *ctx, const char *game_id, const char *kind,
                          const char *src_path)
{
  RhGame *g = NULL;
  GPtrArray *existing = NULL;
  gchar *dir = NULL, *dest = NULL, *base = NULL;
  RhErr e;
  gint64 size;

  if (!ctx || !ctx->db)
    return rh_fail(RH_ERR_DB, "manage: database is not open");
  if (!game_id || !src_path || !kind_ok(kind))
    return rh_fail(RH_ERR_CONFIG, "manage_add_media: bad kind or arguments");
  if (!rh_fs_is_file(src_path))
    return rh_fail(RH_ERR_IO, "media source is not a file: %s", src_path);

  e = store_game_get(ctx, game_id, &g);
  if (!rh_err_ok(&e))
    return e;

  dir = game_dir(ctx, g, &e);
  if (!dir)
    goto out;
  gchar *sub = g_build_filename(dir, kind, NULL);
  if (!rh_fs_mkdir_p(sub, &e)) {
    g_free(sub);
    goto out;
  }

  base = g_path_get_basename(src_path);
  dest = g_build_filename(sub, base, NULL);
  g_free(sub);

  if (rh_fs_copy_atomic(src_path, dest, TRUE, &e) == 1)
    goto out;

  size = rh_fs_size(dest);
  e = store_media_list(ctx, game_id, kind, &existing);
  if (!rh_err_ok(&e))
    goto out;
  e = store_media_add_sized(ctx, game_id, kind, base, size, (int) existing->len);

out:
  if (existing)
    g_ptr_array_unref(existing);
  rh_game_free(g);
  g_free(dir);
  g_free(dest);
  g_free(base);
  return e;
}

RhErr rh_manage_remove_media(RhCtx *ctx, const char *game_id, const char *kind,
                             const char *filename, gboolean delete_file)
{
  RhGame *g = NULL;
  gchar *dir = NULL, *path = NULL;
  RhErr e;

  if (!ctx || !ctx->db)
    return rh_fail(RH_ERR_DB, "manage: database is not open");
  if (!game_id || !filename || !kind_ok(kind))
    return rh_fail(RH_ERR_CONFIG, "manage_remove_media: bad arguments");

  e = store_media_remove(ctx, game_id, kind, filename);
  if (!rh_err_ok(&e))
    return e;

  if (delete_file) {
    e = store_game_get(ctx, game_id, &g);
    if (!rh_err_ok(&e))
      return e;
    dir = game_dir(ctx, g, &e);
    if (dir) {
      path = g_build_filename(dir, kind, filename, NULL);
      if (g_unlink(path) != 0 && errno != ENOENT)
        e = rh_fail(RH_ERR_IO, "failed to remove media file: %s", path);
    }
  }
  rh_game_free(g);
  g_free(dir);
  g_free(path);
  return e;
}

RhErr rh_manage_integrity(RhCtx *ctx, RhIntegrity *out)
{
  RhFilter f = { .multi = -1, .has_patch = -1, .sort = "name" };
  GPtrArray *games = NULL;
  RhErr e;

  if (!ctx || !ctx->db)
    return rh_fail(RH_ERR_DB, "manage: database is not open");
  if (!out)
    return rh_fail(RH_ERR_CONFIG, "manage_integrity: bad arguments");
  memset(out, 0, sizeof(*out));
  out->details = g_ptr_array_new_with_free_func(g_free);

  e = store_game_list(ctx, &f, &games);
  if (!rh_err_ok(&e))
    return e;

  for (guint i = 0; i < games->len; i++) {
    RhGame *g = g_ptr_array_index(games, i);
    gchar *dir = game_dir(ctx, g, &e);
    if (!dir) {
      g_ptr_array_add(out->details,
                      g_strdup_printf("%s: bad dir_path", g->id));
      out->missing_dirs++;
      continue;
    }
    out->games_checked++;
    if (!rh_fs_is_dir(dir)) {
      out->missing_dirs++;
      g_ptr_array_add(out->details,
                      g_strdup_printf("%s: dir missing (%s)", g->id, dir));
      g_free(dir);
      continue;
    }

    GPtrArray *roms = NULL;
    store_rom_list(ctx, g->id, &roms);
    if (roms) {
      gchar *romdir = g_build_filename(dir, "rom", NULL);
      for (guint j = 0; j < roms->len; j++) {
        RhRomFile *r = g_ptr_array_index(roms, j);
        gchar *p = g_build_filename(romdir, r->filename, NULL);
        if (!rh_fs_is_file(p)) {
          out->missing_roms++;
          g_ptr_array_add(out->details,
                          g_strdup_printf("%s: rom missing (%s)", g->id,
                                          r->filename));
        }
        g_free(p);
      }
      g_free(romdir);
      g_ptr_array_unref(roms);
    }

    for (guint k = 0; k < G_N_ELEMENTS(MANAGE_KINDS); k++) {
      GPtrArray *media = NULL;
      store_media_list(ctx, g->id, MANAGE_KINDS[k], &media);
      if (!media)
        continue;
      gchar *sub = g_build_filename(dir, MANAGE_KINDS[k], NULL);
      for (guint j = 0; j < media->len; j++) {
        RhMediaFile *m = g_ptr_array_index(media, j);
        gchar *p = g_build_filename(sub, m->filename, NULL);
        if (!rh_fs_is_file(p)) {
          out->missing_media++;
          g_ptr_array_add(out->details,
                          g_strdup_printf("%s: %s missing (%s)", g->id,
                                          MANAGE_KINDS[k], m->filename));
        }
        g_free(p);
      }
      g_free(sub);
      g_ptr_array_unref(media);
    }
    g_free(dir);
  }
  g_ptr_array_unref(games);
  return rh_ok();
}

void rh_integrity_clear(RhIntegrity *r)
{
  if (!r)
    return;
  if (r->details)
    g_ptr_array_unref(r->details);
  memset(r, 0, sizeof(*r));
}
