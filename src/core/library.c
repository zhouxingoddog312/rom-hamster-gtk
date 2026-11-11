/*
 * library.c — 整库导入（ARCHITECTURE §5 library）
 *
 * 识别 ES / ES-DE 的 gamelist.xml 与 Pegasus 的 metadata.pegasus.txt，
 * 校验清单里的 ROM 真实存在后批量建记录，并把封面/横幅/视频收进库内。
 * 不移动清单目录里已有的 ROM 之外的文件；所有文字只走 ui->log。
 */
#include "library.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <stdio.h>
#include <string.h>

#include "platform.h"
#include "rh-config.h"
#include "rh-fs.h"
#include "rh-hash.h"
#include "rh-str.h"
#include "store.h"

typedef struct {
  gchar *rel;          /* 清单里的 ROM 相对路径 */
  gchar *name;
  gchar *desc;
  gchar *image;
  gchar *marquee;
  gchar *video;
  gchar *developer;
  gchar *publisher;
  gchar *genre;
  int    year;
  double rating;
} LibEntry;

static LibEntry *lib_entry_new(void)
{
  LibEntry *e = g_new0(LibEntry, 1);

  e->rating = -1;
  return e;
}

static void lib_entry_free(LibEntry *e)
{
  if (!e)
    return;
  g_free(e->rel);
  g_free(e->name);
  g_free(e->desc);
  g_free(e->image);
  g_free(e->marquee);
  g_free(e->video);
  g_free(e->developer);
  g_free(e->publisher);
  g_free(e->genre);
  g_free(e);
}

static const gchar *xml_child(xmlNode *node, const char *name)
{
  for (xmlNode *c = node->children; c; c = c->next) {
    if (c->type == XML_ELEMENT_NODE && g_strcmp0((const char *) c->name, name) == 0)
      return (const gchar *) xmlNodeGetContent(c);   /* 调用方负责 xmlFree */
  }
  return NULL;
}

static gchar *xml_child_dup(xmlNode *node, const char *name)
{
  const gchar *s = xml_child(node, name);
  gchar *out = s ? g_strdup(s) : NULL;

  if (s)
    xmlFree((void *) s);
  return out;
}

static void parse_gamelist(const char *path, GPtrArray *out)
{
  xmlDoc *doc = xmlReadFile(path, NULL, XML_PARSE_NONET | XML_PARSE_NOERROR |
                                             XML_PARSE_NOWARNING);

  if (!doc)
    return;
  xmlNode *root = xmlDocGetRootElement(doc);

  for (xmlNode *n = root ? root->children : NULL; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE || g_strcmp0((const char *) n->name, "game") != 0)
      continue;
    {
      LibEntry *e = lib_entry_new();
      gchar *year = xml_child_dup(n, "year");
      gchar *rating = xml_child_dup(n, "rating");

      e->rel = xml_child_dup(n, "path");
      if (e->rel && g_str_has_prefix(e->rel, "./"))
        memmove(e->rel, e->rel + 2, strlen(e->rel + 2) + 1);
      e->name = xml_child_dup(n, "name");
      e->desc = xml_child_dup(n, "desc");
      e->image = xml_child_dup(n, "image");
      e->marquee = xml_child_dup(n, "marquee");
      e->video = xml_child_dup(n, "video");
      e->developer = xml_child_dup(n, "developer");
      e->publisher = xml_child_dup(n, "publisher");
      e->genre = xml_child_dup(n, "genre");
      if (year)
        e->year = atoi(year);
      if (rating) {
        e->rating = g_ascii_strtod(rating, NULL);
        if (e->rating > 1.0 && e->rating <= 10.0)
          e->rating /= 10.0;
      }
      g_free(year);
      g_free(rating);
      if (e->rel && *e->rel)
        g_ptr_array_add(out, e);
      else
        lib_entry_free(e);
    }
  }
  xmlFreeDoc(doc);
}

static void pegasus_set(gchar **slot, const char *line, const char *key)
{
  const char *v = line + strlen(key);

  while (*v == ' ')
    v++;
  g_free(*slot);
  *slot = g_strdup(v);
}

static void parse_pegasus(const char *path, GPtrArray *out)
{
  gchar *text = NULL;

  if (!g_file_get_contents(path, &text, NULL, NULL))
    return;
  LibEntry *cur = NULL;
  gchar **lines = g_strsplit(text, "\n", -1);

  for (guint i = 0; lines[i]; i++) {
    gchar *line = g_strchomp(lines[i]);
    gchar *trim = g_strstrip(line);

    if (g_str_has_prefix(trim, "game:") || g_strcmp0(trim, "game") == 0) {
      cur = lib_entry_new();
      continue;
    }
    if (!cur) {
      if (g_str_has_prefix(trim, "collection:") || g_str_has_prefix(trim, "collection "))
        continue;
      continue;
    }
    if (!*trim) {
      if (cur->rel && *cur->rel)
        g_ptr_array_add(out, cur);
      else
        lib_entry_free(cur);
      cur = NULL;
      continue;
    }
    if (g_str_has_prefix(trim, "file:"))
      pegasus_set(&cur->rel, trim, "file:");
    else if (g_str_has_prefix(trim, "title:"))
      pegasus_set(&cur->name, trim, "title:");
    else if (g_str_has_prefix(trim, "description:"))
      pegasus_set(&cur->desc, trim, "description:");
    else if (g_str_has_prefix(trim, "developer:"))
      pegasus_set(&cur->developer, trim, "developer:");
    else if (g_str_has_prefix(trim, "publisher:"))
      pegasus_set(&cur->publisher, trim, "publisher:");
    else if (g_str_has_prefix(trim, "genre:"))
      pegasus_set(&cur->genre, trim, "genre:");
    else if (g_str_has_prefix(trim, "assets.boxFront:"))
      pegasus_set(&cur->image, trim, "assets.boxFront:");
    else if (g_str_has_prefix(trim, "assets.marquee:"))
      pegasus_set(&cur->marquee, trim, "assets.marquee:");
    else if (g_str_has_prefix(trim, "assets.video:"))
      pegasus_set(&cur->video, trim, "assets.video:");
    else if (g_str_has_prefix(trim, "year:"))
      cur->year = atoi(trim + 5);
    else if (g_str_has_prefix(trim, "rating:")) {
      cur->rating = g_ascii_strtod(trim + 7, NULL);
      if (cur->rating > 1.0 && cur->rating <= 10.0)
        cur->rating /= 10.0;
    }
  }
  if (cur) {
    if (cur->rel && *cur->rel)
      g_ptr_array_add(out, cur);
    else
      lib_entry_free(cur);
  }
  g_strfreev(lines);
  g_free(text);
}

static const gchar *const lib_subdirs[] = {
  "rom", "covers", "screenshots", "videos", "marquees", "miximages", "intro", "patcher"
};

static GPtrArray *lib_scan_roms(const char *romdir)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  GDir *d = g_dir_open(romdir, 0, NULL);
  const gchar *n;

  if (!d)
    return out;
  while ((n = g_dir_read_name(d))) {
    if (!g_str_has_suffix(n, ".m3u"))
      g_ptr_array_add(out, g_strdup(n));
  }
  g_dir_close(d);
  g_ptr_array_sort(out, (GCompareFunc) g_strcmp0);
  return out;
}

static void lib_write_m3u(const char *gdir, const char *main, GPtrArray *roms)
{
  gchar *romdir = g_build_filename(gdir, "rom", NULL);
  gchar *file = NULL;
  GString *buf;

  file = g_strdup_printf("%s/%s.m3u", romdir, main);
  if (roms->len <= 1) {
    g_unlink(file);
    g_free(file);
    g_free(romdir);
    return;
  }
  buf = g_string_new(NULL);
  for (guint i = 0; i < roms->len; i++)
    g_string_append_printf(buf, "%s\n", (const gchar *) roms->pdata[i]);
  {
    RhErr e = rh_ok();

    rh_fs_write_text_atomic(file, buf->str, &e);
  }
  g_string_free(buf, TRUE);
  g_free(file);
  g_free(romdir);
}

/* 一条记录一个事务：游戏 + 全部 roms + 本次新增媒体 */
static RhErr lib_commit(RhCtx *ctx, RhGame *game, const char *romdir,
                        GPtrArray *roms, GPtrArray *media)
{
  RhErr e = store_tx_begin(ctx);

  if (!rh_err_ok(&e))
    return e;
  e = store_game_upsert(ctx, game);
  if (!rh_err_ok(&e)) {
    store_tx_rollback(ctx);
    return e;
  }
  {
    GPtrArray *old = NULL;
    RhErr le = store_rom_list(ctx, game->id, &old);

    if (rh_err_ok(&le) && old) {
      for (guint i = 0; i < old->len; i++) {
        RhRomFile *of = old->pdata[i];
        gboolean keep = FALSE;

        for (guint k = 0; k < roms->len; k++)
          if (g_strcmp0(of->filename, roms->pdata[k]) == 0) {
            keep = TRUE;
            break;
          }
        if (!keep)
          store_rom_remove(ctx, game->id, of->filename);
      }
      g_ptr_array_unref(old);
    }
  }
  for (guint i = 0; i < roms->len; i++) {
    RhRomFile *r = rh_rom_file_new();
    gchar *full = g_build_filename(romdir, roms->pdata[i], NULL);

    r->filename = g_strdup(roms->pdata[i]);
    r->size = rh_fs_size(full);
    r->sort = (int) i;
    {
      gchar *stem = rh_base_noext(roms->pdata[i]);
      gchar *m = NULL;

      r->disc_no = rh_parse_disc_no(stem, &m);
      g_free(m);
      g_free(stem);
    }
    store_rom_remove(ctx, game->id, r->filename);
    {
      RhErr re = store_rom_add(ctx, game->id, r);

      if (!rh_err_ok(&re))
        e = re;
    }
    g_free(full);
    rh_rom_file_free(r);
  }
  for (guint i = 0; media && i < media->len; i++) {
    RhMediaFile *mf = media->pdata[i];
    RhErr me = store_media_add_sized(ctx, game->id, mf->kind, mf->filename,
                                     mf->size, mf->sort);

    if (!rh_err_ok(&me))
      e = me;
  }
  if (!rh_err_ok(&e)) {
    store_tx_rollback(ctx);
    return e;
  }
  return store_tx_commit(ctx);
}

static void lib_copy_media(const char *gdir, const char *main,
                           const char *src, const char *kind,
                           const char *suffix, GPtrArray *media,
                           RhErr *err)
{
  if (!src || !*src || !g_file_test(src, G_FILE_TEST_IS_REGULAR))
    return;
  {
    const gchar *sub = NULL;
    gchar *oe, *name, *mdir, *dest;
    int cp;

    if (g_strcmp0(kind, "covers") == 0)
      sub = "covers";
    else if (g_strcmp0(kind, "marquees") == 0)
      sub = "marquees";
    else if (g_strcmp0(kind, "videos") == 0)
      sub = "videos";
    if (!sub)
      return;
    oe = rh_ext_of(src);
    name = g_strdup_printf("%s-%s%s", main, suffix, oe ? oe : "");
    mdir = g_build_filename(gdir, sub, NULL);
    dest = g_build_filename(mdir, name, NULL);
    cp = rh_fs_copy_atomic(src, dest, TRUE, err);
    if (cp == 0 || cp == 2 || cp == 3) {
      RhMediaFile *mf = rh_media_file_new();

      mf->kind = g_strdup(kind);
      mf->filename = g_strdup(name);
      mf->size = rh_fs_size(dest);
      g_ptr_array_add(media, mf);
    }
    g_free(dest);
    g_free(mdir);
    g_free(name);
    g_free(oe);
  }
}

RhErr rh_library_detect(const char *dir, GPtrArray **manifests)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);

  if (!out)
    return rh_fail(RH_ERR_INTERNAL, "cannot allocate manifest list");
  *manifests = out;
  if (!dir || !g_file_test(dir, G_FILE_TEST_IS_DIR))
    return rh_ok();

  {
    GQueue q = G_QUEUE_INIT;
    g_queue_push_tail(&q, g_strdup(dir));
    int level = 0;

    while (level <= 3 && !g_queue_is_empty(&q)) {
      int n = (int) g_queue_get_length(&q);

      for (int i = 0; i < n; i++) {
        gchar *d = g_queue_pop_head(&q);
        gchar *gl = g_build_filename(d, "gamelist.xml", NULL);
        gchar *pg = g_build_filename(d, "metadata.pegasus.txt", NULL);
        gchar *pe = g_build_filename(d, "metadata.pegasus", NULL);

        if (g_file_test(gl, G_FILE_TEST_EXISTS))
          g_ptr_array_add(out, g_strdup(gl));
        else if (g_file_test(pg, G_FILE_TEST_EXISTS))
          g_ptr_array_add(out, g_strdup(pg));
        else if (g_file_test(pe, G_FILE_TEST_EXISTS))
          g_ptr_array_add(out, g_strdup(pe));
        g_free(gl);
        g_free(pg);
        g_free(pe);

        if (level < 3) {
          GDir *dd = g_dir_open(d, 0, NULL);
          const gchar *nm;

          if (dd) {
            while ((nm = g_dir_read_name(dd))) {
              gchar *sub = g_build_filename(d, nm, NULL);

              if (rh_fs_is_dir(sub))
                g_queue_push_tail(&q, sub);
              else
                g_free(sub);
            }
            g_dir_close(dd);
          }
        }
        g_free(d);
      }
      level++;
    }
    while (!g_queue_is_empty(&q))
      g_free(g_queue_pop_head(&q));
  }
  return rh_ok();
}

RhErr rh_library_import(RhCtx *ctx, RhPlatformRules *rules, const char *dir,
                       const RhImportOpts *opts, RhImportReport **out)
{
  GPtrArray *manifests = NULL;
  RhImportReport *rep;
  RhErr e;
  const gchar *cfg_root;
  gchar *root;

  e = rh_library_detect(dir, &manifests);
  if (!rh_err_ok(&e))
    return e;
  if (!manifests || manifests->len == 0) {
    if (manifests)
      g_ptr_array_unref(manifests);
    return rh_fail(RH_ERR_CONFIG, "no gamelist.xml / metadata.pegasus.txt in %s", dir);
  }

  cfg_root = rh_config_get(ctx->config, "ROM_ROOT");
  root = g_strdup(opts && opts->rom_root && *opts->rom_root
                  ? opts->rom_root
                  : (cfg_root ? cfg_root : ""));
  rep = g_new0(RhImportReport, 1);
  for (guint mi = 0; mi < manifests->len; mi++) {
    gchar *manifest = manifests->pdata[mi];
    gchar *sysdir = g_path_get_dirname(manifest);
    gchar *sysname = g_path_get_basename(sysdir);
    gchar *base = g_path_get_basename(manifest);
    gboolean pegasus = g_str_has_prefix(base, "metadata.");
    GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify) lib_entry_free);
    RhPlatform *rp = platform_resolve_dir(rules, sysname);
    const gchar *platform = rp ? rp->id : NULL;

    rh_log(ctx, RH_LOG_INFO, "整库清单：%s（%s）", manifest,
           pegasus ? "pegasus" : "es");
    if (pegasus)
      parse_pegasus(manifest, entries);
    else
      parse_gamelist(manifest, entries);

    for (guint i = 0; i < entries->len; i++) {
      LibEntry *le = entries->pdata[i];
      gchar *rom = g_build_filename(sysdir, le->rel, NULL);
      gchar *ext, *main, *san, *dir_path, *gdir, *romdir, *dest;
      gchar *game_id, *base_name;
      RhGame *game = NULL;
      GPtrArray *roms = NULL, *media = NULL;
      const char *plat = platform;
      RhErr te;

      if (!g_file_test(rom, G_FILE_TEST_IS_REGULAR)) {
        rh_log(ctx, RH_LOG_WARN, "清单中的 ROM 不存在，跳过：%s", le->rel);
        rep->skipped++;
        g_free(rom);
        continue;
      }
      ext = rh_ext_of(rom);
      base_name = rh_rom_base_name(rom, ext);
      main = g_strdup(le->name && *le->name ? le->name : base_name);
      /* 碟号取自源文件名（"X (Disc 1).gdi"），目录名用清单里的 <name>；
       * 不能只靠目录里已有文件推断，否则第一张碟会落成 "X.gdi"。 */
      guint dno = 0;
      {
        gchar *fn_main = NULL;

        dno = (guint) rh_parse_disc_no(base_name, &fn_main);
        g_free(fn_main);
      }
      if (!plat) {
        GPtrArray *cand = platform_by_extension(rules, ext);

        if (cand && cand->len > 0)
          plat = ((RhPlatform *) cand->pdata[0])->id;
      }
      if (!plat) {
        rh_log(ctx, RH_LOG_WARN, "无法确定平台，跳过：%s", le->rel);
        rep->skipped++;
        g_free(main);
        g_free(base_name);
        g_free(ext);
        g_free(rom);
        continue;
      }

      san = rh_sanitize_dirname(main);
      dir_path = g_strdup_printf("%s/%s", plat, san);
      gdir = g_build_filename(root, dir_path, NULL);
      romdir = g_build_filename(gdir, "rom", NULL);
      for (guint k = 0; k < G_N_ELEMENTS(lib_subdirs); k++) {
        gchar *sub = g_build_filename(gdir, lib_subdirs[k], NULL);
        RhErr me = rh_ok();

        rh_fs_mkdir_p(sub, &me);
        g_free(sub);
      }
      game_id = rh_game_gen_id(plat, dir_path);
      if (store_game_get(ctx, game_id, &game).code != RH_OK)
        game = NULL;
      roms = lib_scan_roms(romdir);
      {
        gboolean multi = FALSE;
        gchar *tname;

        for (guint k = 0; k < roms->len && !multi; k++) {
          gchar *st = rh_base_noext(roms->pdata[k]);
          gchar *mm = NULL;

          if (rh_parse_disc_no(st, &mm) > 0)
            multi = TRUE;
          g_free(mm);
          g_free(st);
        }
        if (game && game->is_multi_disc)
          multi = TRUE;
        if (dno > 0)
          multi = TRUE;
        if (multi) {
          tname = g_strdup_printf("%s (%s %u)%s", main,
                                  rh_config_get(ctx->config, "disc_label_prefix") ?
                                  rh_config_get(ctx->config, "disc_label_prefix") : "Disc",
                                  dno > 0 ? dno : roms->len + 1, ext ? ext : "");
        } else {
          tname = g_strdup_printf("%s%s", main, ext ? ext : "");
        }
        dest = g_build_filename(romdir, tname, NULL);
        {
          int cp = rh_fs_copy_atomic(rom, dest, opts ? opts->overwrite : FALSE, &te);

          if (cp == 1) {
            rh_log(ctx, RH_LOG_ERROR, "复制失败：%s（%s）", rom, te.msg);
            rep->failed++;
            g_free(dest);
            g_free(tname);
            goto next_entry;
          }
          if (cp == 3) {
            rh_log(ctx, RH_LOG_WARN, "目标已存在且不同，跳过：%s", tname);
            rep->skipped++;
            g_free(dest);
            g_free(tname);
            goto next_entry;
          }
        }
        g_ptr_array_unref(roms);
        roms = lib_scan_roms(romdir);
        lib_write_m3u(gdir, main, roms);

        if (!game) {
          game = rh_game_new();
          game->id = g_strdup(game_id);
          game->platform = g_strdup(plat);
          game->dir_path = g_strdup(dir_path);
          game->game_main_name = g_strdup(main);
          game->name = g_strdup(le->name && *le->name ? le->name : main);
          game->description = g_strdup(le->desc);
          game->developer = g_strdup(le->developer);
          game->publisher = g_strdup(le->publisher);
          game->genre = g_strdup(le->genre);
          game->year = le->year;
          game->rating = le->rating;
          game->rom_status = g_strdup("unknown");
          game->patch_type = g_strdup("");
          game->created_at = rh_now_iso();
        }
        if (le->name && *le->name && g_strcmp0(game->name, le->name) != 0) {
          g_free(game->name);
          game->name = g_strdup(le->name);
        }
        if (le->rating >= 0)
          game->rating = le->rating;
        g_free(game->updated_at);
        game->updated_at = rh_now_iso();
        game->is_multi_disc = multi || roms->len > 1;
        game->is_arcade = platform_get(rules, plat) ? platform_get(rules, plat)->arcade : FALSE;
        g_free(game->rom_format);
        game->rom_format = g_strdup(g_str_equal(ext, ".zip") ? "zip" :
                                    g_str_equal(ext, ".7z") ? "7z" : "raw");

        media = g_ptr_array_new_with_free_func((GDestroyNotify) rh_media_file_free);
        {
          gchar *img = le->image ? g_build_filename(sysdir, le->image, NULL) : NULL;
          gchar *mar = le->marquee ? g_build_filename(sysdir, le->marquee, NULL) : NULL;
          gchar *vid = le->video ? g_build_filename(sysdir, le->video, NULL) : NULL;

          lib_copy_media(gdir, main, img, "covers", "image", media, &te);
          lib_copy_media(gdir, main, mar, "marquees", "marquee", media, &te);
          lib_copy_media(gdir, main, vid, "videos", "video", media, &te);
          g_free(img);
          g_free(mar);
          g_free(vid);
        }

        te = lib_commit(ctx, game, romdir, roms, media);
        if (rh_err_ok(&te)) {
          rep->imported++;
          rep->planned++;
        } else {
          rh_log(ctx, RH_LOG_ERROR, "落库失败：%s（%s）", le->rel, te.msg);
          rep->failed++;
        }
        g_free(dest);
        g_free(tname);
      }

next_entry:
      if (media)
        g_ptr_array_unref(media);
      if (roms)
        g_ptr_array_unref(roms);
      rh_game_free(game);
      g_free(game_id);
      g_free(romdir);
      g_free(gdir);
      g_free(dir_path);
      g_free(san);
      g_free(main);
      g_free(base_name);
      g_free(ext);
      g_free(rom);
    }

    g_ptr_array_unref(entries);
    g_free(base);
    g_free(sysname);
    g_free(sysdir);
  }

  g_ptr_array_unref(manifests);
  g_free(root);
  *out = rep;
  return rh_ok();
}
