/*
 * import.c — 导入编排实现
 *
 * 两阶段（ARCHITECTURE §6.2）：
 *   scan   ：递归遍历输入，把压缩包六级分流/平台判定/重复与媒体归属的交互
 *            全部做完，产出一份"已决策"的条目计划；
 *   execute：逐条搬文件 + 单条事务落库，不发起任何交互（可放工作线程）。
 * 文件侧顺序（§11）：复制到临时名 → rename 到最终名 → 再提交事务。
 */
#include "import.h"

#include <glib/gstdio.h>
#include <string.h>

#include "classify.h"
#include "dat.h"
#include "job.h"
#include "rh-archive.h"
#include "rh-fs.h"
#include "rh-hash.h"
#include "library.h"
#include "rh-records.h"
#include "rh-str.h"
#include "store.h"

typedef struct {
  int      seq;
  gchar   *src;
  gchar   *action;        /* rom / media */
  gchar   *platform;
  gchar   *main;
  int      disc;
  gchar   *ext;
  gchar   *media_kind;
  gchar   *target_id;     /* media 的归属游戏 */
  gboolean keep_archive;
  gboolean arcade;
  gchar   *rom_format;
  gboolean original;
} RhPlanItem;

typedef struct {
  RhCtx               *ctx;
  RhPlatformRules     *rules;
  const RhImportOpts  *opts;
  gchar               *rom_root;
  gchar               *dat_dir;
  gchar               *tmp_root;
  gchar               *tmp_cleanup;
  GPtrArray           *items;
  GHashTable          *dat_loaded;
  int                  skipped;
  int                  extract_seq;
  gchar               *pkg_platform;   /* 当前解压包的 ROM 归属上下文 */
  gchar               *pkg_main;
  GHashTable          *lib_done;      /* 已按整库处理过的输入目录 */
  int                  lib_imported;
  int                  lib_skipped;
  int                  lib_failed;
} Scan;

/* ------------------------------------------------------------ 计划条目 */

static RhPlanItem *item_new(const char *src, const char *action)
{
  RhPlanItem *it = g_new0(RhPlanItem, 1);

  it->src = g_strdup(src);
  it->action = g_strdup(action);
  return it;
}

static void item_free(RhPlanItem *it)
{
  if (!it)
    return;
  g_free(it->src);
  g_free(it->action);
  g_free(it->platform);
  g_free(it->main);
  g_free(it->ext);
  g_free(it->media_kind);
  g_free(it->target_id);
  g_free(it->rom_format);
  g_free(it);
}

static const gchar *fmt_of_ext(const char *ext)
{
  if (!ext)
    return "raw";
  if (g_ascii_strcasecmp(ext, ".zip") == 0)
    return "zip";
  if (g_ascii_strcasecmp(ext, ".7z") == 0)
    return "7z";
  if (g_ascii_strcasecmp(ext, ".chd") == 0)
    return "chd";
  return "raw";
}

/* ------------------------------------------------------------ 交互封装 */

static int ask_choose(RhCtx *ctx, const char *title, const char **items, int n,
                      const char *key)
{
  if (!ctx->ui || !ctx->ui->choose)
    return -1;
  return ctx->ui->choose(ctx->ui->userdata, title, items, n, key);
}

/* ------------------------------------------------------------ 查询助手 */

static gboolean ext_is_ambiguous(RhPlatformRules *rules, const char *ext,
                                 gchar **platform_out)
{
  GPtrArray *all = platform_all(rules);
  const gchar *fallback = NULL;

  /* 只有列在 ambiguous_formats 里的后缀才算二义。
   * 多个平台都声明二义时，优先“该后缀同时是它主扩展名”的平台
   * （.32x -> sega32x，而不是把 .32x 当歧义的 megadrive）。 */
  for (guint i = 0; i < all->len; i++) {
    RhPlatform *p = all->pdata[i];
    gboolean ambiguous = FALSE;
    gboolean primary = FALSE;

    for (guint j = 0; p->ambiguous_formats && j < p->ambiguous_formats->len; j++)
      if (g_ascii_strcasecmp(p->ambiguous_formats->pdata[j], ext) == 0) {
        ambiguous = TRUE;
        break;
      }
    if (!ambiguous)
      continue;
    for (guint j = 0; p->extensions && j < p->extensions->len; j++)
      if (g_ascii_strcasecmp(p->extensions->pdata[j], ext) == 0) {
        primary = TRUE;
        break;
      }
    if (primary) {
      if (platform_out)
        *platform_out = g_strdup(p->id);
      return TRUE;
    }
    if (!fallback)
      fallback = p->id;
  }
  if (fallback) {
    if (platform_out)
      *platform_out = g_strdup(fallback);
    return TRUE;
  }
  return FALSE;
}

/* 同平台 + 名称相似度 ≥ 阈值；返回值由调用方 rh_game_free */
static RhGame *find_duplicate(Scan *s, const char *platform, const char *main,
                              int *sim_out)
{
  RhFilter *f = rh_filter_new();
  GPtrArray *games = NULL;
  RhGame *best = NULL;
  int best_sim = -1;
  int threshold = rh_config_get_int(s->ctx->config,
                                    "duplicate_check_threshold", 70);
  RhErr e;

  f->platform = platform;
  e = store_game_list(s->ctx, f, &games);
  rh_filter_free(f);
  if (!rh_err_ok(&e) || !games)
    return NULL;

  for (guint i = 0; i < games->len; i++) {
    RhGame *g = games->pdata[i];
    const gchar *nm = g->game_main_name ? g->game_main_name : g->name;
    int sim = rh_name_sim(main, nm);

    if (sim >= threshold && sim > best_sim) {
      best_sim = sim;
      best = rh_game_copy(g);
    }
  }
  g_ptr_array_unref(games);
  if (sim_out)
    *sim_out = best_sim;
  return best;
}

static RhGame *find_game_for_media(Scan *s, const char *base,
                                   const char *platform_hint)
{
  RhFilter *f = rh_filter_new();
  GPtrArray *games = NULL;
  RhGame *best = NULL;
  int best_sim = -1;
  RhErr e;

  if (platform_hint)
    f->platform = platform_hint;
  e = store_game_list(s->ctx, f, &games);
  rh_filter_free(f);
  if (!rh_err_ok(&e) || !games)
    return NULL;

  for (guint i = 0; i < games->len; i++) {
    RhGame *g = games->pdata[i];
    const gchar *nm = g->game_main_name ? g->game_main_name : g->name;
    int sim = rh_name_sim(base, nm);

    if (sim >= 60 && sim > best_sim) {
      best_sim = sim;
      best = rh_game_copy(g);
    }
  }
  g_ptr_array_unref(games);
  return best;
}

/* ------------------------------------------------------------ 归档统计 */

typedef struct {
  int n, nested;
  int patch, rom, ambig, doc, arcade_suf, img, video, other;
  int psv;
  gchar *platform;
  gchar *single_rom;
  gchar *ambig_files;
  gchar *ambig_platform;
} ArcStats;

static void arc_stats_clear(ArcStats *st)
{
  g_free(st->platform);
  g_free(st->single_rom);
  g_free(st->ambig_files);
  g_free(st->ambig_platform);
  memset(st, 0, sizeof *st);
}

static gboolean psv_name(const char *name)
{
  return strstr(name, "sce_sys") || strstr(name, "PCSH") ||
         strstr(name, "PCSF") || strstr(name, "PCSE") ||
         strstr(name, "PCSG") || strstr(name, "PCSD") ||
         strstr(name, "PCSA");
}

static int scan_archive_entries(Scan *s, const char *path, ArcStats *st)
{
  GPtrArray *entries = NULL;
  RhErr e;

  memset(st, 0, sizeof *st);
  if (rh_arc_list(path, NULL, &entries, &e) != 0 || !entries)
    return 0;

  for (guint i = 0; i < entries->len; i++) {
    RhArcEntry *en = entries->pdata[i];
    gchar *ext;
    const gchar *mk;
    GPtrArray *plats;
    gchar *ap = NULL;

    if (en->is_dir)
      continue;
    if (strchr(en->name, '/'))
      st->nested = 1;
    st->n++;
    if (psv_name(en->name))
      st->psv = 1;

    ext = rh_ext_of(en->name);
    if (!ext || !*ext) {
      g_free(ext);
      st->other++;
      continue;
    }

    mk = platform_media_kind(s->rules, en->name);
    if (mk && strcmp(mk, RH_MEDIA_KIND_PATCHER) == 0) {
      st->patch++;
      g_free(ext);
      continue;
    }
    if (g_ascii_strcasecmp(ext, ".txt") == 0 ||
        g_ascii_strcasecmp(ext, ".md") == 0 ||
        g_ascii_strcasecmp(ext, ".nfo") == 0) {
      st->doc++;
      g_free(ext);
      continue;
    }
    if (platform_is_arcade_rom_suffix(s->rules, ext)) {
      st->arcade_suf++;
      st->rom++;
      if (!st->single_rom)
        st->single_rom = g_strdup(en->name);
      g_free(ext);
      continue;
    }
    if (ext_is_ambiguous(s->rules, ext, &ap)) {
      st->ambig++;
      st->rom++;
      if (ap && !st->ambig_platform)
        st->ambig_platform = g_strdup(ap);
      {
        gchar *base = g_path_get_basename(en->name);
        gchar *joined = g_strdup_printf("%s%s ",
                                        st->ambig_files ? st->ambig_files : "",
                                        base);

        g_free(st->ambig_files);
        st->ambig_files = joined;
        g_free(base);
      }
      g_free(ap);
      g_free(ext);
      continue;
    }
    plats = platform_by_extension(s->rules, ext);
    if (plats && plats->len > 0) {
      st->rom++;
      if (!st->single_rom)
        st->single_rom = g_strdup(en->name);
      if (!st->platform)
        st->platform = g_strdup(((RhPlatform *) plats->pdata[0])->id);
      g_free(ext);
      continue;
    }
    if (mk && strcmp(mk, RH_MEDIA_KIND_VIDEOS) == 0)
      st->video++;
    else if (mk && (strcmp(mk, RH_MEDIA_KIND_COVERS) == 0 ||
                    strcmp(mk, RH_MEDIA_KIND_SCREENSHOTS) == 0 ||
                    strcmp(mk, RH_MEDIA_KIND_MARQUEES) == 0 ||
                    strcmp(mk, RH_MEDIA_KIND_MIXIMAGES) == 0))
      st->img++;
    else
      st->other++;
    g_free(ext);
  }
  g_ptr_array_unref(entries);
  return st->n;
}

static gchar *extract_dir(Scan *s, const char *archive)
{
  gchar *sub = g_strdup_printf("x-%d", ++s->extract_seq);
  gchar *dir = g_build_filename(s->tmp_root, sub, NULL);
  RhErr e;

  g_free(sub);
  rh_err_clear(&e);
  if (rh_arc_extract(archive, dir, NULL, &e) != 0) {
    rh_log(s->ctx, RH_LOG_WARN, "解压失败：%s（%s）", archive, e.msg);
    g_free(dir);
    return NULL;
  }
  return dir;
}

static void plan_path(Scan *s, const char *path, gboolean original, int depth);
static void plan_media_file(Scan *s, const char *path, const char *kind,
                            const char *platform_hint);

static void plan_extracted(Scan *s, const char *dir)
{
  GPtrArray *files = rh_fs_walk_files(dir);
  gchar *pf = NULL, *pm = NULL;

  /* 先确定包内 ROM 的平台与主名，供同包的封面/截图等媒体绑定 */
  for (guint i = 0; i < files->len && !pf; i++) {
    RhClass *c = rh_class_new();
    RhErr e = rh_ok();

    if (rh_classify_file(s->rules, files->pdata[i], c, &e) == 0 &&
        c->kind == RH_CLASS_ROM) {
      const gchar *plat = c->platform;

      if (!plat && c->candidates->len > 0)
        plat = c->candidates->pdata[0];
      if (plat) {
        gchar *stem = rh_rom_base_name(files->pdata[i], c->ext);
        gchar *main = NULL;

        rh_parse_disc_no(stem, &main);
        pf = g_strdup(plat);
        pm = g_strdup(main && *main ? main : stem);
        g_free(main);
        g_free(stem);
      }
    }
    rh_class_free(c);
  }

  g_free(s->pkg_platform);
  g_free(s->pkg_main);
  s->pkg_platform = pf;
  s->pkg_main = pm;

  for (guint i = 0; i < files->len; i++)
    plan_path(s, files->pdata[i], FALSE, 3);

  g_free(s->pkg_platform);
  g_free(s->pkg_main);
  s->pkg_platform = NULL;
  s->pkg_main = NULL;
  g_ptr_array_unref(files);
}

static void plan_archive(Scan *s, const char *path)
{
  ArcStats st;
  gchar *ext = rh_ext_of(path);
  int idx = -1;

  if (!scan_archive_entries(s, path, &st) || st.n == 0) {
    rh_log(s->ctx, RH_LOG_WARN, "压缩包为空或无法读取，跳过：%s", path);
    s->skipped++;
    goto done;
  }

  /* 〇级：PSV 整库（整体入库，不解压） */
  if (st.psv) {
    RhPlanItem *it = item_new(path, "rom");

    it->platform = g_strdup("psvita");
    it->ext = g_strdup(ext);
    it->main = rh_rom_base_name(path, ext);
    it->keep_archive = TRUE;
    it->rom_format = g_strdup(fmt_of_ext(ext));
    it->original = TRUE;
    g_ptr_array_add(s->items, it);
    goto done;
  }

  /* 一级：纯补丁包（补丁占比 ≥80%，无 ROM/媒体/二义） */
  if (st.ambig == 0 && st.patch > 0 && st.rom == 0 &&
      (st.img + st.video + st.other) == 0 && st.patch * 100 / st.n >= 80) {
    rh_log(s->ctx, RH_LOG_INFO, "纯补丁包，整体保留：%s", path);
    plan_media_file(s, path, RH_MEDIA_KIND_PATCHER, NULL);
    goto done;
  }

  /* 二级：街机标准包（.zip/.7z，根目录平铺 + 街机后缀 ≥95%） */
  if (ext && (g_ascii_strcasecmp(ext, ".zip") == 0 ||
              g_ascii_strcasecmp(ext, ".7z") == 0) && !st.nested &&
      st.arcade_suf > 0 && st.arcade_suf * 100 / st.n >= 95) {
    if (s->opts->non_interactive) {
      idx = 0;
    } else {
      const gchar *opts[3] = { "直接导入（保留原生 ZIP）", "解压后导入", "跳过" };
      idx = ask_choose(s->ctx, "街机 ROM 压缩包处理方式", opts, 3, "archive_mode");
      if (idx < 0)
        idx = 2;
    }
    if (idx == 0) {
      RhPlanItem *it = item_new(path, "rom");

      it->ext = g_strdup(ext);
      it->main = rh_rom_base_name(path, ext);
      it->arcade = TRUE;
      it->keep_archive = TRUE;
      it->rom_format = g_strdup(ext[0] == '.' ? ext + 1 : ext);
      it->platform = g_strdup("mame");
      it->original = TRUE;
      g_ptr_array_add(s->items, it);
      goto done;
    }
    if (idx == 1) {
      gchar *dir = extract_dir(s, path);

      if (dir) {
        plan_extracted(s, dir);
        g_free(dir);
      }
      goto done;
    }
    s->skipped++;
    goto done;
  }

  /* 三级：二义性主体 */
  if (st.ambig > 0) {
    gboolean guess_patch = FALSE;
    int choice;

    {
      gchar *low = g_ascii_strdown(st.ambig_files ? st.ambig_files : "", -1);

      guess_patch = strstr(low, "patch") || strstr(low, "dlc") ||
                    strstr(low, "update") || strstr(low, "crack");
      g_free(low);
    }
    if (s->opts->non_interactive) {
      choice = guess_patch ? 1 : 0;
    } else {
      const gchar *opts[4] = {
        "作为游戏本体压缩包导入",
        "作为补丁/DLC 压缩包导入（整体保留）",
        "解压后逐个分类处理",
        "跳过",
      };
      choice = ask_choose(s->ctx, "包含二义性格式文件", opts, 4, "ambiguous_archive");
      if (choice < 0)
        choice = 3;
    }
    if (choice == 1) {
      plan_media_file(s, path, RH_MEDIA_KIND_PATCHER, NULL);
      goto done;
    }
    if (choice == 0) {
      RhPlanItem *it;
      const gchar *plat = st.platform ? st.platform : st.ambig_platform;

      if (!plat) {
        rh_log(s->ctx, RH_LOG_WARN, "二义性压缩包无法确定平台，跳过：%s", path);
        s->skipped++;
        goto done;
      }
      it = item_new(path, "rom");
      it->ext = g_strdup(ext);
      it->main = rh_rom_base_name(path, ext);
      it->platform = g_strdup(plat);
      it->keep_archive = TRUE;
      it->rom_format = g_strdup(fmt_of_ext(ext));
      it->original = TRUE;
      g_ptr_array_add(s->items, it);
      goto done;
    }
    if (choice == 2) {
      gchar *dir = extract_dir(s, path);

      if (dir) {
        plan_extracted(s, dir);
        g_free(dir);
      }
      goto done;
    }
    s->skipped++;
    goto done;
  }

  /* 四级：单 ROM 包 + 平台支持直读 */
  if (st.rom == 1 && (st.doc + st.img + st.video + st.other) == 0 &&
      !st.nested && st.platform) {
    RhPlatform *p = platform_get(s->rules, st.platform);

    if (p && p->compress_supported) {
      int mode = 0;

      if (!s->opts->non_interactive) {
        const gchar *opts[2] = { "直接导入（保留压缩包）", "解压后导入" };
        mode = ask_choose(s->ctx, "单 ROM 压缩包处理方式", opts, 2, "archive_mode");
        if (mode < 0)
          mode = 1;
      }
      if (mode == 0) {
        RhPlanItem *it;

        if (!st.platform) {
          rh_log(s->ctx, RH_LOG_WARN, "单 ROM 包平台未知，跳过：%s", path);
          s->skipped++;
          goto done;
        }
        it = item_new(path, "rom");
        it->ext = g_strdup(ext);
        it->main = rh_rom_base_name(path, ext);
        it->platform = g_strdup(st.platform);
        it->keep_archive = TRUE;
        it->rom_format = g_strdup(fmt_of_ext(ext));
        it->original = TRUE;
        g_ptr_array_add(s->items, it);
        goto done;
      }
      {
        gchar *dir = extract_dir(s, path);

        if (dir) {
          plan_extracted(s, dir);
          g_free(dir);
        }
      }
      goto done;
    }
  }

  /* 五级：混合资源包 */
  if (st.rom > 0 || st.patch > 0 || st.img > 0 || st.video > 0) {
    gchar *dir = extract_dir(s, path);

    if (dir) {
      plan_extracted(s, dir);
      g_free(dir);
    }
    goto done;
  }

  rh_log(s->ctx, RH_LOG_WARN, "无法识别压缩包内容，跳过：%s", path);
  s->skipped++;

done:
  arc_stats_clear(&st);
  g_free(ext);
}

/* ------------------------------------------------------------ 单文件 */

static void plan_rom_file(Scan *s, const char *path, const char *platform,
                          gboolean arcade, gboolean keep, const char *ext)
{
  RhPlanItem *it;
  gchar *stem = rh_rom_base_name(path, ext);
  gchar *main = NULL;
  int disc;
  RhGame *dup = NULL;

  disc = rh_parse_disc_no(stem, &main);
  if (!main || !*main) {
    g_free(main);
    main = g_strdup(stem);
  }
  g_free(stem);

  it = item_new(path, "rom");
  it->platform = g_strdup(platform);
  it->ext = g_strdup(ext);
  it->main = main;
  it->disc = disc;
  it->arcade = arcade;
  it->keep_archive = keep;
  it->rom_format = g_strdup(keep ? fmt_of_ext(ext) : "raw");
  it->original = TRUE;

  dup = find_duplicate(s, platform, main, NULL);
  if (dup) {
    int action;

    if (s->opts->non_interactive) {
      action = disc > 0 ? 0 : 1;
    } else {
      const gchar *opts[3] = { "追加为多碟/多 ROM", "新建游戏", "跳过该文件" };
      gchar *title = g_strdup_printf("疑似重复游戏：%s", main);

      action = ask_choose(s->ctx, title, opts, 3, "duplicate");
      g_free(title);
      if (action < 0)
        action = 2;
    }
    if (action == 0) {
      it->target_id = g_strdup(dup->id);
    } else if (action == 2) {
      s->skipped++;
      rh_game_free(dup);
      item_free(it);
      return;
    }
    rh_game_free(dup);
  }
  g_ptr_array_add(s->items, it);
}

static void plan_media_file(Scan *s, const char *path, const char *kind,
                            const char *platform_hint)
{
  gchar *base = rh_base_noext(path);
  RhGame *game = find_game_for_media(s, base, platform_hint);
  RhPlanItem *it;

  g_free(base);
  if (!game) {
    const gchar *plat = platform_hint ? platform_hint : s->pkg_platform;

    if (plat && s->pkg_main) {
      /* 同包 ROM 尚未入库：先记下归属，执行阶段再按 platform/主名 找游戏 */
      it = item_new(path, "media");
      it->media_kind = g_strdup(kind);
      it->platform = g_strdup(plat);
      it->main = g_strdup(s->pkg_main);
      it->original = TRUE;
      g_ptr_array_add(s->items, it);
      return;
    }
    rh_log(s->ctx, RH_LOG_WARN, "找不到归属游戏，跳过媒体：%s", path);
    s->skipped++;
    return;
  }
  it = item_new(path, "media");
  it->media_kind = g_strdup(kind);
  it->target_id = g_strdup(game->id);
  it->platform = g_strdup(game->platform);
  it->main = g_strdup(game->game_main_name ? game->game_main_name : game->name);
  it->original = TRUE;
  g_ptr_array_add(s->items, it);
  rh_game_free(game);
}

static void plan_path(Scan *s, const char *path, gboolean original, int depth)
{
  RhClass *c;
  RhErr e;

  if (depth > 6) {
    rh_log(s->ctx, RH_LOG_WARN, "嵌套过深，跳过：%s", path);
    s->skipped++;
    return;
  }
  if (rh_fs_is_dir(path)) {
    GPtrArray *files = rh_fs_walk_files(path);

    for (guint i = 0; i < files->len; i++)
      plan_path(s, files->pdata[i], original, depth + 1);
    g_ptr_array_unref(files);
    return;
  }
  if (!rh_fs_is_file(path)) {
    s->skipped++;
    return;
  }

  c = rh_class_new();
  rh_err_clear(&e);
  if (rh_classify_file(s->rules, path, c, &e) != 0) {
    rh_log(s->ctx, RH_LOG_DEBUG, "%s", e.msg);
    s->skipped++;
    rh_class_free(c);
    return;
  }

  switch (c->kind) {
  case RH_CLASS_ARCHIVE:
    plan_archive(s, path);
    break;

  case RH_CLASS_PATCH_GENERIC:
  case RH_CLASS_PATCH_PLATFORM:
    plan_media_file(s, path, RH_MEDIA_KIND_PATCHER, c->platform);
    break;

  case RH_CLASS_AMBIGUOUS: {
    int choice;

    if (s->opts->non_interactive) {
      choice = 0;
    } else {
      const gchar *opts[3] = { "作为游戏本体", "作为补丁/DLC", "跳过" };
      gchar *title = g_strdup_printf("二义性格式：%s", c->ext);

      choice = ask_choose(s->ctx, title, opts, 3, "ambiguous");
      g_free(title);
      if (choice < 0)
        choice = 2;
    }
    if (choice == 0)
      plan_rom_file(s, path, c->platform, FALSE, FALSE, c->ext);
    else if (choice == 1)
      plan_media_file(s, path, RH_MEDIA_KIND_PATCHER, c->platform);
    else
      s->skipped++;
    break;
  }

  case RH_CLASS_ROM: {
    const gchar *plat = c->platform;
    gchar *chosen = NULL;

    if (!plat && c->candidates->len > 1) {
      if (s->opts->non_interactive) {
        plat = c->candidates->pdata[0];
      } else {
        const gchar **opts = g_new0(const gchar *, c->candidates->len + 1);

        for (guint i = 0; i < c->candidates->len; i++)
          opts[i] = c->candidates->pdata[i];
        {
          int idx = ask_choose(s->ctx, "该扩展名属于多个平台，请选择",
                               opts, (int) c->candidates->len, "platform");

          if (idx >= 0) {
            chosen = g_strdup(opts[idx]);
            plat = chosen;
          }
        }
        g_free(opts);
      }
    }
    if (!plat) {
      rh_log(s->ctx, RH_LOG_WARN, "无法确定平台，跳过：%s", path);
      s->skipped++;
    } else {
      plan_rom_file(s, path, plat, FALSE, FALSE, c->ext);
    }
    g_free(chosen);
    break;
  }

  case RH_CLASS_MEDIA:
    plan_media_file(s, path, c->media_kind, NULL);
    break;

  default:
    s->skipped++;
    break;
  }
  rh_class_free(c);
  (void) original;
}

/* ------------------------------------------------------------ 执行 */

static const gchar *const subdirs[] = {
  "rom", "covers", "screenshots", "videos", "marquees", "miximages", "intro", "patcher"
};

static int ensure_game_dirs(const char *root, const char *dir_path, RhErr *err)
{
  gchar *gdir = g_build_filename(root, dir_path, NULL);
  int ok = 1;

  for (guint i = 0; i < G_N_ELEMENTS(subdirs) && ok; i++) {
    gchar *sub = g_build_filename(gdir, subdirs[i], NULL);

    if (!rh_fs_mkdir_p(sub, err))
      ok = 0;
    g_free(sub);
  }
  g_free(gdir);
  return ok;
}

static gboolean is_arcade_platform(Scan *s, const char *platform)
{
  RhPlatform *p = platform_get(s->rules, platform);

  return p ? p->arcade : FALSE;
}

static GPtrArray *scan_rom_dir(const char *romdir)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  GDir *d = g_dir_open(romdir, 0, NULL);
  const gchar *name;

  if (!d)
    return out;
  while ((name = g_dir_read_name(d))) {
    gchar *full;

    if (g_str_has_suffix(name, ".m3u"))
      continue;
    full = g_build_filename(romdir, name, NULL);
    if (rh_fs_is_file(full))
      g_ptr_array_add(out, g_strdup(name));
    g_free(full);
  }
  g_dir_close(d);
  g_ptr_array_sort(out, (GCompareFunc) g_strcmp0);
  return out;
}

static gint cmp_disc_order(gconstpointer a, gconstpointer b)
{
  const gchar *fa = *(const gchar * const *) a;
  const gchar *fb = *(const gchar * const *) b;
  gchar *ma = NULL, *mb = NULL;
  int da = rh_parse_disc_no(rh_base_noext(fa), &ma);
  int db = rh_parse_disc_no(rh_base_noext(fb), &mb);
  gint r;

  if (da == 0) da = 999;
  if (db == 0) db = 999;
  r = da - db;
  if (r == 0)
    r = g_strcmp0(fa, fb);
  g_free(ma);
  g_free(mb);
  return r;
}

static void rebuild_m3u(Scan *s, const char *gdir, const char *main,
                        gboolean arcade, GPtrArray *roms, RhErr *err)
{
  (void) s;
  gchar *romdir = g_build_filename(gdir, "rom", NULL);
  gchar *m3u = g_strdup_printf("%s/%s.m3u", romdir, main);

  if (arcade || roms->len <= 1) {
    g_unlink(m3u);
    g_free(m3u);
    g_free(romdir);
    return;
  }
  {
    GString *buf = g_string_new(NULL);
    gchar *text;

    g_ptr_array_sort(roms, cmp_disc_order);
    for (guint i = 0; i < roms->len; i++)
      g_string_append_printf(buf, "%s\n", (const gchar *) roms->pdata[i]);
    text = g_string_free(buf, FALSE);
    rh_fs_write_text_atomic(m3u, text, err);
    g_free(text);
  }
  g_free(m3u);
  g_free(romdir);
}

static gchar *target_filename(Scan *s, RhPlanItem *it, RhGame *existing,
                              GPtrArray *roms, gboolean *became_multi,
                              gboolean *rename_single, gchar **old_single)
{
  const gchar *prefix = rh_config_get(s->ctx->config, "disc_label_prefix");
  gchar *ext = it->keep_archive ? rh_ext_of(it->src)
                                : g_strdup(it->ext ? it->ext : "");

  if (!prefix || !*prefix)
    prefix = "Disc";
  *became_multi = FALSE;
  *rename_single = FALSE;
  *old_single = NULL;

  if (it->arcade) {
    gchar *base = g_path_get_basename(it->src);

    g_free(ext);
    return base;
  }

  if (it->disc > 0 || (existing && existing->is_multi_disc)) {
    int disc = it->disc;

    if (existing && !existing->is_multi_disc && roms->len == 1) {
      *rename_single = TRUE;
      *old_single = g_strdup(roms->pdata[0]);
    }
    if (disc == 0)
      disc = (int) roms->len + 1;
    *became_multi = TRUE;
    {
      gchar *out = g_strdup_printf("%s (%s %d)%s", it->main, prefix, disc, ext);

      g_free(ext);
      return out;
    }
  }
  {
    gchar *out = g_strdup_printf("%s%s", it->main, ext ? ext : "");

    g_free(ext);
    return out;
  }
}

static void ensure_dat(Scan *s, const char *platform)
{
  gchar *name, *path;

  if (!s->dat_dir || !*s->dat_dir || !platform)
    return;
  if (g_hash_table_contains(s->dat_loaded, platform))
    return;
  g_hash_table_add(s->dat_loaded, g_strdup(platform));

  name = g_strdup_printf("%s.dat", platform);
  path = g_build_filename(s->dat_dir, name, NULL);
  if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    gchar *upper = g_strdup_printf("%s.DAT", platform);
    gchar *alt = g_build_filename(s->dat_dir, upper, NULL);

    if (g_file_test(alt, G_FILE_TEST_EXISTS)) {
      g_free(path);
      path = alt;
    } else {
      g_free(alt);
    }
    g_free(upper);
  }
  if (g_file_test(path, G_FILE_TEST_EXISTS)) {
    RhErr e = rh_dat_import_file(s->ctx, platform, path, NULL);

    if (!rh_err_ok(&e))
      rh_log(s->ctx, RH_LOG_WARN, "DAT 导入失败（%s）：%s", platform, e.msg);
  }
  g_free(path);
  g_free(name);
}

/* 单条事务：记录 + roms + job_items 状态同事务（ARCHITECTURE §11） */
static RhErr store_item_tx(Scan *s, RhGame *game, const char *romdir,
                           GPtrArray *roms, const char *target, const RhHash *h,
                           gint64 job_id, int seq)
{
  RhErr e = store_tx_begin(s->ctx);

  if (!rh_err_ok(&e))
    return e;

  e = store_game_upsert(s->ctx, game);
  if (!rh_err_ok(&e)) {
    store_tx_rollback(s->ctx);
    return e;
  }
  /* 同名改名（单碟升级为 Disc 1）后旧行会残留，按当前目录实际文件集合清理 */
  {
    GPtrArray *old = NULL;
    RhErr      le = store_rom_list(s->ctx, game->id, &old);

    if (rh_err_ok(&le) && old) {
      for (guint i = 0; i < old->len; i++) {
        RhRomFile *of = old->pdata[i];
        gboolean   keep = FALSE;

        for (guint k = 0; k < roms->len; k++)
          if (g_strcmp0(of->filename, roms->pdata[k]) == 0) {
            keep = TRUE;
            break;
          }
        if (!keep)
          store_rom_remove(s->ctx, game->id, of->filename);
      }
      g_ptr_array_unref(old);
    }
  }
  for (guint i = 0; i < roms->len; i++) {
    const gchar *fn = roms->pdata[i];
    RhRomFile *r = rh_rom_file_new();
    gchar *stem = rh_base_noext(fn);
    gchar *m = NULL;
    gchar *full = g_build_filename(romdir, fn, NULL);
    RhErr re;

    r->filename = g_strdup(fn);
    r->disc_no = rh_parse_disc_no(stem, &m);
    r->size = rh_fs_size(full);
    if (target && strcmp(fn, target) == 0 && h)
      r->crc32 = g_strdup(h->crc32);
    r->sort = (int) i;

    (void) store_rom_remove(s->ctx, game->id, fn);
    re = store_rom_add(s->ctx, game->id, r);
    if (!rh_err_ok(&re))
      e = re;

    g_free(full);
    g_free(m);
    g_free(stem);
    rh_rom_file_free(r);
    if (!rh_err_ok(&e))
      break;
  }
  if (rh_err_ok(&e))
    e = store_job_item_update(s->ctx, job_id, seq, "done", NULL);
  if (!rh_err_ok(&e)) {
    store_tx_rollback(s->ctx);
    return e;
  }
  return store_tx_commit(s->ctx);
}

/* ROM 先执行、媒体后执行：媒体要绑定同一批次刚入库的游戏（对齐 bash 两阶段） */
static void order_items(GPtrArray *items)
{
  GPtrArray *roms = g_ptr_array_new();
  GPtrArray *media = g_ptr_array_new();

  for (guint i = 0; items && i < items->len; i++) {
    RhPlanItem *it = items->pdata[i];

    if (it->action && strcmp(it->action, "rom") == 0)
      g_ptr_array_add(roms, it);
    else
      g_ptr_array_add(media, it);
  }
  g_ptr_array_set_free_func(items, NULL);   /* 只搬指针，不能让旧数组释放元素 */
  g_ptr_array_set_size(items, 0);
  g_ptr_array_set_free_func(items, (GDestroyNotify) item_free);
  for (guint i = 0; i < roms->len; i++)
    g_ptr_array_add(items, roms->pdata[i]);
  for (guint i = 0; i < media->len; i++)
    g_ptr_array_add(items, media->pdata[i]);
  g_ptr_array_unref(roms);
  g_ptr_array_unref(media);
}

static int exec_rom(Scan *s, RhPlanItem *it, RhImportReport *rep)
{
  RhErr e;
  gchar *san = rh_sanitize_dirname(it->main);
  gchar *dir_path = g_strdup_printf("%s/%s", it->platform, san);
  gchar *gdir = g_build_filename(s->rom_root, dir_path, NULL);
  gchar *romdir = g_build_filename(gdir, "rom", NULL);
  gchar *game_id = rh_game_gen_id(it->platform, dir_path);
  RhGame *game = NULL;
  GPtrArray *roms = NULL;
  int rc = 1;
  RhHash h;

  memset(&h, 0, sizeof h);
  h.size = -1;
  rh_err_clear(&e);

  if (!ensure_game_dirs(s->rom_root, dir_path, &e))
    goto done;
  if (store_game_get(s->ctx, game_id, &game).code != RH_OK)
    game = NULL;

  roms = scan_rom_dir(romdir);
  {
    gboolean became_multi = FALSE, rename_single = FALSE;
    gchar *old_single = NULL;
    gchar *target = target_filename(s, it, game, roms, &became_multi,
                                    &rename_single, &old_single);
    gchar *dest = g_build_filename(romdir, target, NULL);

    if (rename_single && old_single) {
      const gchar *prefix = rh_config_get(s->ctx->config, "disc_label_prefix");
      gchar *oe = rh_ext_of(old_single);
      gchar *to_name = g_strdup_printf("%s (%s 1)%s", it->main,
                                       prefix && *prefix ? prefix : "Disc", oe);
      gchar *from = g_build_filename(romdir, old_single, NULL);
      gchar *to = g_build_filename(romdir, to_name, NULL);

      if (g_file_test(from, G_FILE_TEST_EXISTS))
        g_rename(from, to);
      g_free(to);
      g_free(from);
      g_free(to_name);
      g_free(oe);
    }

    {
      int cp = rh_fs_copy_atomic(it->src, dest, s->opts->overwrite, &e);

      if (cp == 1) {
        rh_log(s->ctx, RH_LOG_ERROR, "复制失败：%s（%s）", it->src, e.msg);
        g_free(dest);
        g_free(target);
        g_free(old_single);
        goto done;
      }
      if (cp == 3) {
        rh_log(s->ctx, RH_LOG_WARN, "目标已存在且不同，跳过：%s", target);
        rep->skipped++;
        g_free(dest);
        g_free(target);
        g_free(old_single);
        rc = 0;
        goto done;
      }
    }

    /* 文件已就位，再刷新列表/哈希/记录，最后提交事务 */
    g_ptr_array_unref(roms);
    roms = scan_rom_dir(romdir);
    ensure_dat(s, it->platform);
    rebuild_m3u(s, gdir, it->main,
                it->arcade || is_arcade_platform(s, it->platform), roms, &e);

    (void) rh_hash_file(dest, FALSE, FALSE, &h, NULL);

    if (!game) {
      RhDatMatch *m = NULL;

      rh_dat_match_rom(s->ctx, it->platform, h.crc32, NULL, NULL, &m);
      game = rh_game_new();
      game->id = g_strdup(game_id);
      game->platform = g_strdup(it->platform);
      game->dir_path = g_strdup(dir_path);
      game->game_main_name = g_strdup(it->main);
      game->name = g_strdup(m && m->game_name ? m->game_name : it->main);
      game->rom_status = g_strdup(m ? "good" : "unknown");
      game->dat_source = g_strdup(m ? m->dat_name : NULL);
      game->patch_type = g_strdup("");
      game->rating = -1;
      game->created_at = rh_now_iso();
      rh_dat_match_free(m);
    } else {
      RhDatMatch *m = NULL;

      rh_dat_match_rom(s->ctx, it->platform, h.crc32, NULL, NULL, &m);
      if (m && m->game_name) {
        g_free(game->name);
        game->name = g_strdup(m->game_name);
      }
      if (m) {
        g_free(game->rom_status);
        game->rom_status = g_strdup("good");
        g_free(game->dat_source);
        game->dat_source = g_strdup(m->dat_name);
      }
      rh_dat_match_free(m);
    }
    g_free(game->updated_at);
    game->updated_at = rh_now_iso();
    game->is_multi_disc = became_multi || it->disc > 0 || roms->len > 1;
    game->is_arcade = it->arcade || is_arcade_platform(s, it->platform);
    g_free(game->rom_format);
    game->rom_format = g_strdup(it->rom_format ? it->rom_format : "raw");
    if (!game->game_main_name)
      game->game_main_name = g_strdup(it->main);

    {
      RhErr tx = store_item_tx(s, game, romdir, roms, target, &h,
                               rep->job_id, it->seq);

      if (!rh_err_ok(&tx)) {
        rh_log(s->ctx, RH_LOG_ERROR, "落库失败：%s", tx.msg);
        rh_hash_clear(&h);
        g_free(dest);
        g_free(target);
        g_free(old_single);
        goto done;
      }
    }
    rep->imported++;
    if (it->original && rh_config_get_bool(s->ctx->config, "auto_delete_source"))
      g_unlink(it->src);

    g_free(dest);
    g_free(target);
    g_free(old_single);
  }
  rc = 0;

done:
  rh_hash_clear(&h);
  if (roms)
    g_ptr_array_unref(roms);
  rh_game_free(game);
  g_free(romdir);
  g_free(gdir);
  g_free(dir_path);
  g_free(san);
  g_free(game_id);
  return rc;
}

static int exec_media(Scan *s, RhPlanItem *it, RhImportReport *rep)
{
  RhGame *g = NULL;
  gchar *gdir = NULL, *mdir = NULL, *base = NULL, *dest = NULL;
  RhErr e;
  int rc = 1;

  rh_err_clear(&e);
  {
    const gchar *tid = it->target_id;
    gchar *computed = NULL;

    if (!tid && it->platform && it->main) {
      gchar *san = rh_sanitize_dirname(it->main);
      gchar *dp = g_strdup_printf("%s/%s", it->platform, san);

      computed = rh_game_gen_id(it->platform, dp);
      tid = computed;
      g_free(dp);
      g_free(san);
    }
    if (!tid || store_game_get(s->ctx, tid, &g).code != RH_OK || !g) {
      rh_log(s->ctx, RH_LOG_WARN, "归属游戏尚不存在，跳过媒体：%s", it->src);
      g_free(computed);
      rep->skipped++;
      return 0;
    }
    g_free(computed);
  }
  gdir = g_build_filename(s->rom_root, g->dir_path, NULL);
  mdir = g_build_filename(gdir, platform_media_dir(s->rules, it->media_kind), NULL);
  base = g_path_get_basename(it->src);
  dest = g_build_filename(mdir, base, NULL);

  {
    int cp = rh_fs_copy_atomic(it->src, dest, s->opts->overwrite, &e);

    if (cp == 1) {
      rh_log(s->ctx, RH_LOG_ERROR, "复制失败：%s（%s）", it->src, e.msg);
      goto done;
    }
    if (cp == 3) {
      rep->skipped++;
      rc = 0;
      goto done;
    }
  }

  {
    RhErr tx = store_tx_begin(s->ctx);

    if (!rh_err_ok(&tx)) {
      rh_log(s->ctx, RH_LOG_ERROR, "事务开启失败：%s", tx.msg);
      goto done;
    }
    {
      RhErr a = store_media_add_sized(s->ctx, g->id, it->media_kind, base,
                                      rh_fs_size(dest), 0);
      RhErr b = store_job_item_update(s->ctx, rep->job_id, it->seq, "done", NULL);

      if (!rh_err_ok(&a) || !rh_err_ok(&b)) {
        store_tx_rollback(s->ctx);
        rh_log(s->ctx, RH_LOG_ERROR, "落库失败：%s",
               !rh_err_ok(&a) ? a.msg : b.msg);
        goto done;
      }
    }
    store_tx_commit(s->ctx);
  }
  rep->media++;
  rc = 0;

done:
  rh_game_free(g);
  g_free(dest);
  g_free(base);
  g_free(mdir);
  g_free(gdir);
  return rc;
}

/* ------------------------------------------------------------ 入口 */

RhErr rh_import_run(RhCtx *ctx, RhPlatformRules *rules, GPtrArray *inputs,
                    const RhImportOpts *opts, RhImportReport **out)
{
  Scan s;
  RhImportReport *rep = NULL;
  RhErr ret = rh_ok();
  const gchar *root_cfg;
  GPtrArray *view = NULL;

  g_return_val_if_fail(out != NULL, rh_fail(RH_ERR_INTERNAL, "import: out is NULL"));
  *out = NULL;

  memset(&s, 0, sizeof s);
  s.ctx = ctx;
  s.rules = rules;
  s.opts = opts ? opts : &(RhImportOpts){ 0 };
  s.items = g_ptr_array_new_with_free_func((GDestroyNotify) item_free);
  s.dat_loaded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  root_cfg = s.opts->rom_root;
  if ((!root_cfg || !*root_cfg) && ctx->config)
    root_cfg = rh_config_get(ctx->config, "ROM_ROOT");
  if (!root_cfg || !*root_cfg) {
    ret = rh_fail(RH_ERR_CONFIG,
                  "ROM_ROOT 未配置：请先 rom-hamster init --root <目录>");
    goto cleanup;
  }
  s.rom_root = g_canonicalize_filename(root_cfg, NULL);

  {
    const gchar *dd = ctx->config ? rh_config_get(ctx->config, "dat_dir") : NULL;

    if (dd && *dd) {
      gchar *canon = g_canonicalize_filename(dd, NULL);

      s.dat_dir = g_canonicalize_filename(canon, NULL);
      g_free(canon);
    }
  }

  {
    const gchar *env = g_get_tmp_dir();

    s.tmp_cleanup = g_strdup_printf("%s/romham-import-XXXXXX", env);
    s.tmp_root = g_mkdtemp(s.tmp_cleanup);
    if (!s.tmp_root) {
      ret = rh_fail(RH_ERR_IO, "无法创建工作临时目录");
      goto cleanup;
    }
  }

  /* 第一阶段：扫描（全部交互在此） */
  rh_progress(ctx, -1, "扫描输入");
  s.lib_done = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; inputs && i < inputs->len; i++) {
    const gchar *in = inputs->pdata[i];

    if (rh_fs_is_dir(in)) {
      GPtrArray *mf = NULL;

      if (rh_library_detect(in, &mf).code == RH_OK && mf && mf->len > 0) {
        RhImportReport *lr = NULL;
        RhErr le = rh_ok();

        if (!s.opts->dry_run)
          le = rh_library_import(ctx, rules, in, s.opts, &lr);
        if (rh_err_ok(&le) && lr) {
          s.lib_imported += lr->imported;
          s.lib_skipped += lr->skipped;
          s.lib_failed += lr->failed;
          g_free(lr);
        } else if (!rh_err_ok(&le)) {
          rh_log(ctx, RH_LOG_ERROR, "整库导入失败：%s（%s）", in, le.msg);
          s.lib_failed++;
        }
        g_hash_table_add(s.lib_done, g_strdup(in));
        g_ptr_array_unref(mf);
        continue;
      }
      if (mf)
        g_ptr_array_unref(mf);
    }
    plan_path(&s, in, TRUE, 0);
  }

  order_items(s.items);

  rep = g_new0(RhImportReport, 1);
  rep->planned = (int) s.items->len + s.lib_imported + s.lib_skipped + s.lib_failed;
  rep->imported = s.lib_imported;
  rep->skipped = s.skipped + s.lib_skipped;
  rep->failed = s.lib_failed;

  if (s.opts->dry_run || rep->planned == 0) {
    *out = rep;
    rep = NULL;
    rh_progress(ctx, 1.0, "扫描完成");
    goto cleanup;
  }

  if (!rh_fs_mkdir_p(s.rom_root, &ret))
    goto cleanup;

  /* 第二阶段：执行（零交互） */
  {
    GPtrArray *jitems = g_ptr_array_new_with_free_func((GDestroyNotify) rh_job_item_free);

    for (guint i = 0; i < s.items->len; i++) {
      RhPlanItem *pi = s.items->pdata[i];
      RhJobItem  *ji = rh_job_item_new();

      ji->src = g_strdup(pi->src);
      ji->action = g_strdup(pi->action);
      ji->platform = g_strdup(pi->platform);
      ji->target_id = g_strdup(pi->target_id);
      g_ptr_array_add(jitems, ji);
    }
    rep->job_id = rh_job_begin(ctx, "import", jitems);
    g_ptr_array_unref(jitems);
  }
  if (rep->job_id <= 0) {
    ret = rh_fail(RH_ERR_DB, "无法创建导入作业");
    goto cleanup;
  }
  {
    RhErr je = rh_job_items(ctx, rep->job_id, &view);

    if (!rh_err_ok(&je))
      view = NULL;
  }

  for (guint i = 0; i < s.items->len; i++) {
    RhPlanItem *it = s.items->pdata[i];
    gboolean done = FALSE;

    if (rh_job_cancelled(ctx)) {
      rh_job_finish(ctx, rep->job_id, "cancelled", NULL);
      ret = rh_ok();
      goto cleanup;
    }
    for (guint k = 0; view && k < view->len; k++) {
      RhJobItem *ji = view->pdata[k];

      if (ji->seq == (int) i + 1 && ji->state && strcmp(ji->state, "done") == 0)
        done = TRUE;
    }
    if (done) {
      rep->skipped++;
      continue;
    }
    if ((it->action && strcmp(it->action, "media") == 0)
          ? exec_media(&s, it, rep)
          : exec_rom(&s, it, rep))
      rep->failed++;

    {
      gchar *base = g_path_get_basename(it->src);

      rh_progress(ctx, (double) (i + 1) / (double) s.items->len, base);
      g_free(base);
    }
  }
  rh_job_finish(ctx, rep->job_id, rep->failed ? "failed" : "done", NULL);
  *out = rep;
  rep = NULL;
  rh_progress(ctx, 1.0, "导入结束");

cleanup:
  if (view)
    g_ptr_array_unref(view);
  if (rep)
    g_free(rep);
  if (s.lib_done)
    g_hash_table_unref(s.lib_done);
  if (s.tmp_root)
    rh_fs_remove_tree(s.tmp_root);
  g_free(s.tmp_cleanup);
  g_free(s.rom_root);
  g_free(s.dat_dir);
  g_hash_table_unref(s.dat_loaded);
  g_ptr_array_unref(s.items);
  return ret;
}
