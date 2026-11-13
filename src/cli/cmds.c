/*
 * 子命令：init / import / dat
 *
 * GOptionContext 把 argv[0] 当程序名，因此子命令参数要先补一个占位 argv[0]。
 */
#include "cmds.h"

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "dat.h"
#include "detail.h"
#include "export.h"
#include "import.h"
#include "manage.h"
#include "platform.h"
#include "query.h"
#include "rh-config.h"
#include "rh-ctx.h"
#include "rh-fs.h"
#include "rh-ui.h"
#include "store.h"

extern const RhUi *ui_cli_get(void);

typedef struct {
  int        argc;
  gchar    **argv;
  GPtrArray *owned;   /* g_option_context_parse 会把已消费的参数从 argv 里移走，
                         不能靠 g_strfreev(argv) 回收，必须自己记账 */
} SubArgs;

static SubArgs sub_args_new(int argc, char **argv)
{
  SubArgs a;

  a.argc = argc + 1;
  a.argv = g_new0(gchar *, (gsize) a.argc + 1);
  a.owned = g_ptr_array_new_with_free_func(g_free);
  a.argv[0] = g_strdup("rom-hamster");
  g_ptr_array_add(a.owned, a.argv[0]);
  for (int i = 0; i < argc; i++) {
    a.argv[i + 1] = g_strdup(argv[i]);
    g_ptr_array_add(a.owned, a.argv[i + 1]);
  }
  return a;
}

static void sub_args_free(SubArgs *a)
{
  if (!a)
    return;
  if (a->owned) {
    g_ptr_array_unref(a->owned);
    a->owned = NULL;
  }
  g_free(a->argv);
  a->argv = NULL;
  a->argc = 0;
}

/* G_GINT64_FORMAT 不能出现在 msgid 里（xgettext 会把宏当成字符串的一部分），
 * 可翻译的格式串统一用 %s 传数值。 */
static const char *i64str(gint64 v, char *buf, gsize size)
{
  g_snprintf(buf, size, "%" G_GINT64_FORMAT, v);
  return buf;
}

static RhCtx *make_ctx(void)
{
  RhCtx *ctx = rh_ctx_new();

  rh_ctx_set_ui(ctx, ui_cli_get());
  return ctx;
}

/* 选项说明用 N_() 标记、由 GOptionContext 在 --help 时翻译（GLib 约定）。 */
static GOptionContext *opt_new(const char *param, GOptionEntry *entries)
{
  GOptionContext *opt = g_option_context_new(param);

  g_option_context_set_translation_domain(opt, GETTEXT_PACKAGE);
  g_option_context_add_main_entries(opt, entries, GETTEXT_PACKAGE);
  return opt;
}

static int open_store_for(RhCtx *ctx, gboolean create)
{
  const gchar *root = rh_config_get(ctx->config, "ROM_ROOT");
  gchar *dbp = rh_ctx_resolve_db_path(ctx, root);
  RhErr e;

  if (!dbp) {
    g_printerr(_("ROM_ROOT is not configured: run rom-hamster init --root <dir>\n"));
    return 1;
  }
  e = store_open(ctx, dbp, create ? STORE_OPEN_CREATE : 0);
  g_free(dbp);
  if (!rh_err_ok(&e)) {
    g_printerr(_("cannot open database: %s\n"), e.msg);
    return 1;
  }
  return 0;
}

int cmd_init(int argc, char **argv)
{
  const char *root = NULL, *dat_dir = NULL;
  GOptionEntry entries[] = {
    { "root", 0, 0, G_OPTION_ARG_STRING, &root,
      N_("ROM root directory"), N_("<dir>") },
    { "dat-dir", 0, 0, G_OPTION_ARG_STRING, &dat_dir,
      N_("DAT directory"), N_("<dir>") },
    { NULL, 0, 0, 0, NULL, NULL, NULL },
  };
  GOptionContext *opt = opt_new(N_("- initialize a ROM library"), entries);
  GError *gerr = NULL;
  SubArgs a = sub_args_new(argc, argv);
  RhCtx *ctx;
  gchar *user_path;
  int rc = 0;

  if (!g_option_context_parse(opt, &a.argc, &a.argv, &gerr)) {
    g_printerr("%s\n", gerr->message);
    g_error_free(gerr);
    g_option_context_free(opt);
    sub_args_free(&a);
    return 2;
  }
  g_option_context_free(opt);
  sub_args_free(&a);

  if (!root || !*root) {
    g_printerr(_("usage: rom-hamster init --root <dir> [--dat-dir <dir>]\n"));
    return 2;
  }

  ctx = make_ctx();
  {
    gchar *canon = g_canonicalize_filename(root, NULL);

    g_mkdir_with_parents(canon, 0755);
    rh_config_set(ctx->config, "ROM_ROOT", canon, RH_LAYER_CLI);
    if (dat_dir && *dat_dir)
      rh_config_set(ctx->config, "dat_dir", dat_dir, RH_LAYER_CLI);

    user_path = rh_config_user_path();
    {
      RhErr e = rh_ok();
      gchar *dir = g_path_get_dirname(user_path);
      gchar *content;

      g_mkdir_with_parents(dir, 0755);
      g_free(dir);
      content = g_strdup_printf("# rom-hamster user configuration\n"
                                "ROM_ROOT=%s\n"
                                "%s%s%s\n",
                                canon,
                                dat_dir && *dat_dir ? "dat_dir=" : "",
                                dat_dir && *dat_dir ? dat_dir : "",
                                dat_dir && *dat_dir ? "" : "");
      if (rh_fs_write_text_atomic(user_path, content, &e) != 0) {
        g_printerr(_("cannot write configuration: %s (%s)\n"), user_path, e.msg);
        rc = 1;
      }
      g_free(content);
    }

    if (rc == 0) {
      if (open_store_for(ctx, TRUE) == 0) {
        g_print(_("ROM_ROOT: %s\n"), canon);
        g_print(_("store:    %s\n"), ctx->db_path);
        g_print(_("config:   %s\n"), user_path);
      } else {
        rc = 1;
      }
    }
    g_free(canon);
    g_free(user_path);
  }
  if (ctx->db)
    store_close(ctx);
  rh_ctx_free(ctx);
  return rc;
}

int cmd_import(int argc, char **argv)
{
  gboolean yes = FALSE, dry = FALSE, overwrite = FALSE;
  const char *root = NULL, *platforms = NULL;
  GOptionEntry entries[] = {
    { "yes", 'y', 0, G_OPTION_ARG_NONE, &yes,
      N_("non-interactive defaults"), NULL },
    { "dry-run", 0, 0, G_OPTION_ARG_NONE, &dry,
      N_("scan only"), NULL },
    { "overwrite", 0, 0, G_OPTION_ARG_NONE, &overwrite,
      N_("overwrite differing files"), NULL },
    { "root", 0, 0, G_OPTION_ARG_STRING, &root,
      N_("override ROM_ROOT"), N_("<dir>") },
    { "platforms", 0, 0, G_OPTION_ARG_STRING, &platforms,
      N_("extra platform rules"), N_("<file>") },
    { NULL, 0, 0, 0, NULL, NULL, NULL },
  };
  GOptionContext *opt = opt_new(N_("<paths> - import ROMs and media"), entries);
  GError *gerr = NULL;
  SubArgs a = sub_args_new(argc, argv);
  RhCtx *ctx;
  RhPlatformRules *rules;
  GPtrArray *inputs;
  RhImportReport *rep = NULL;
  RhErr e = rh_ok();
  int rc = 0;

  if (!g_option_context_parse(opt, &a.argc, &a.argv, &gerr)) {
    g_printerr("%s\n", gerr->message);
    g_error_free(gerr);
    g_option_context_free(opt);
    sub_args_free(&a);
    return 2;
  }
  g_option_context_free(opt);

  ctx = make_ctx();
  if (root && *root)
    rh_config_set(ctx->config, "ROM_ROOT", root, RH_LAYER_CLI);
  if (open_store_for(ctx, TRUE) != 0) {
    sub_args_free(&a);
    rh_ctx_free(ctx);
    return 1;
  }

  rules = platform_rules_new();
  if (platforms && *platforms)
    e = platform_rules_load(rules, platforms);
  if (rh_err_ok(&e))
    e = platform_rules_load_standard(rules);
  if (!rh_err_ok(&e) && platform_count(rules) == 0) {
    g_printerr(_("cannot load platform rules: %s\n"), e.msg);
    platform_rules_free(rules);
    store_close(ctx);
    sub_args_free(&a);
    rh_ctx_free(ctx);
    return 1;
  }

  inputs = g_ptr_array_new_with_free_func(g_free);
  for (int i = 1; i < a.argc; i++)
    g_ptr_array_add(inputs, g_canonicalize_filename(a.argv[i], NULL));

  if (inputs->len == 0) {
    g_printerr(_("at least one file or directory is required\n"));
    rc = 2;
  } else {
    RhImportOpts opts;
    RhImportReport *r = NULL;

    opts.rom_root = NULL;
    opts.non_interactive = yes;
    opts.overwrite = overwrite;
    opts.dry_run = dry;

    e = rh_import_run(ctx, rules, inputs, &opts, &r);
    rep = r;
    if (!rh_err_ok(&e)) {
      g_printerr(_("import failed: %s\n"), e.msg);
      rc = 1;
    }
  }
  if (rep) {
    char jobbuf[32];

    g_print(_("planned %d: imported %d, media %d, skipped %d, failed %d (job %s)\n"),
            rep->planned, rep->imported, rep->media, rep->skipped, rep->failed,
            i64str(rep->job_id, jobbuf, sizeof jobbuf));
    if (rep->failed)
      rc = 1;
    g_free(rep);
  }

  g_ptr_array_unref(inputs);
  platform_rules_free(rules);
  store_close(ctx);
  sub_args_free(&a);
  rh_ctx_free(ctx);
  return rc;
}

int cmd_dat(int argc, char **argv)
{
  RhCtx *ctx;
  RhErr e;
  int rc = 0;

  if (argc < 3 || g_strcmp0(argv[0], "import") != 0) {
    g_printerr(_("usage: rom-hamster dat import <platform> <file.dat>\n"));
    return 2;
  }
  ctx = make_ctx();
  if (open_store_for(ctx, TRUE) != 0) {
    rh_ctx_free(ctx);
    return 1;
  }
  e = rh_dat_import_file(ctx, argv[1], argv[2], NULL);
  if (!rh_err_ok(&e)) {
    g_printerr(_("DAT import failed: %s\n"), e.msg);
    rc = 1;
  } else {
    g_print(_("DAT indexed: %s -> %s\n"), argv[2], argv[1]);
  }
  store_close(ctx);
  rh_ctx_free(ctx);
  return rc;
}

/* ================================================================= M3 CLI */

static int parse_common(GOptionContext *opt, SubArgs *a, const char *usage,
                        int min_pos)
{
  GError *gerr = NULL;

  g_option_context_set_help_enabled(opt, TRUE);
  if (!g_option_context_parse(opt, &a->argc, &a->argv, &gerr)) {
    g_printerr("%s\n", gerr->message);
    g_error_free(gerr);
    return 2;
  }
  if (a->argc < min_pos) {
    g_printerr("%s\n", usage);
    return 2;
  }
  return 0;
}

static void print_games(GPtrArray *games)
{
  g_print(_("id\tname\tplatform\tyear\tgenre\trating\n"));
  for (guint i = 0; i < games->len; i++) {
    RhGame *g = g_ptr_array_index(games, i);
    char rating[16] = "";
    if (g->rating >= 0)
      g_snprintf(rating, sizeof rating, "%.2f", g->rating);
    g_print("%s\t%s\t%s\t%d\t%s\t%s\n", g->id, g->name, g->platform,
            g->year, g->genre ? g->genre : "", rating);
  }
}

int cmd_list(int argc, char **argv)
{
  const char *platform = NULL, *genre = NULL, *developer = NULL;
  const char *publisher = NULL, *region = NULL, *patch_type = NULL;
  const char *status = NULL, *format = NULL, *sort = NULL;
  const char *kw = NULL;
  char **tags = NULL;
  int year_from = 0, year_to = 0;
  gboolean only_multi = FALSE, only_single = FALSE;
  gboolean only_patch = FALSE, only_nopatch = FALSE, desc = FALSE;
  SubArgs a = sub_args_new(argc, argv);
  GOptionEntry entries[] = {
    { "platform", 'p', 0, G_OPTION_ARG_STRING, &platform,
      N_("Platform id"), N_("ID") },
    { "genre", 'g', 0, G_OPTION_ARG_STRING, &genre,
      N_("Genre"), N_("GENRE") },
    { "developer", 'd', 0, G_OPTION_ARG_STRING, &developer,
      N_("Developer"), N_("DEV") },
    { "publisher", 0, 0, G_OPTION_ARG_STRING, &publisher,
      N_("Publisher"), N_("PUB") },
    { "region", 'r', 0, G_OPTION_ARG_STRING, &region,
      N_("Region"), N_("REGION") },
    { "patch-type", 0, 0, G_OPTION_ARG_STRING, &patch_type,
      N_("Patch type"), N_("TYPE") },
    { "year-from", 0, 0, G_OPTION_ARG_INT, &year_from,
      N_("Earliest year"), N_("YEAR") },
    { "year-to", 0, 0, G_OPTION_ARG_INT, &year_to,
      N_("Latest year"), N_("YEAR") },
    { "status", 0, 0, G_OPTION_ARG_STRING, &status,
      N_("ROM status"), N_("STATUS") },
    { "format", 0, 0, G_OPTION_ARG_STRING, &format,
      N_("ROM format"), N_("FMT") },
    { "tag", 't', 0, G_OPTION_ARG_STRING_ARRAY, &tags,
      N_("Tag (repeatable)"), N_("TAG") },
    { "kw", 'k', 0, G_OPTION_ARG_STRING, &kw,
      N_("Keyword (FTS)"), N_("WORD") },
    { "multi", 0, 0, G_OPTION_ARG_NONE, &only_multi,
      N_("Only multi-disc games"), NULL },
    { "single", 0, 0, G_OPTION_ARG_NONE, &only_single,
      N_("Only single-disc games"), NULL },
    { "has-patch", 0, 0, G_OPTION_ARG_NONE, &only_patch,
      N_("Only patched games"), NULL },
    { "no-patch", 0, 0, G_OPTION_ARG_NONE, &only_nopatch,
      N_("Only unpatched games"), NULL },
    { "sort", 's', 0, G_OPTION_ARG_STRING, &sort,
      N_("Sort: name/rating/year/discs"), N_("KEY") },
    { "desc", 0, 0, G_OPTION_ARG_NONE, &desc,
      N_("Descending order"), NULL },
    { NULL, 0, 0, 0, NULL, NULL, NULL },
  };
  GOptionContext *opt = opt_new(N_("- list games"), entries);
  RhFilter f = { .multi = -1, .has_patch = -1, .sort = "name" };
  RhCtx *ctx;
  GPtrArray *games = NULL;
  RhErr e;
  int rc;

  rc = parse_common(opt, &a, _("usage: rom-hamster list [options]"), 1);
  g_option_context_free(opt);
  if (rc) {
    sub_args_free(&a);
    return rc;
  }
  if (only_multi && only_single) {
    g_printerr(_("--multi and --single are mutually exclusive\n"));
    sub_args_free(&a);
    return 2;
  }
  if (only_patch && only_nopatch) {
    g_printerr(_("--has-patch and --no-patch are mutually exclusive\n"));
    sub_args_free(&a);
    return 2;
  }

  f.platform = platform; f.genre = genre; f.developer = developer;
  f.publisher = publisher; f.region = region; f.patch_type = patch_type;
  f.year_from = year_from; f.year_to = year_to;
  f.multi = only_multi ? 1 : (only_single ? 0 : -1);
  f.has_patch = only_patch ? 1 : (only_nopatch ? 0 : -1);
  f.status = status; f.format = format;
  f.tags = (const char **) tags;
  f.n_tags = tags ? (int) g_strv_length(tags) : 0;
  f.kw = kw;
  f.sort = sort ? sort : "name";
  f.desc = desc;

  ctx = make_ctx();
  if (open_store_for(ctx, FALSE) != 0) {
    rh_ctx_free(ctx);
    sub_args_free(&a);
    return 1;
  }
  e = rh_query_list(ctx, &f, &games);
  if (!rh_err_ok(&e)) {
    g_printerr(_("query failed: %s\n"), e.msg);
    rc = 1;
  } else {
    print_games(games);
    g_print(g_dngettext(GETTEXT_PACKAGE, "%u game\n", "%u games\n", games->len), games->len);
  }
  if (games)
    g_ptr_array_unref(games);
  store_close(ctx);
  rh_ctx_free(ctx);
  sub_args_free(&a);
  return rc;
}

int cmd_search(int argc, char **argv)
{
  const char *kw = NULL;
  SubArgs a = sub_args_new(argc, argv);
  GOptionEntry entries[] = {
    { "kw", 'k', 0, G_OPTION_ARG_STRING, &kw,
      N_("Keyword"), N_("WORD") },
    { NULL, 0, 0, 0, NULL, NULL, NULL },
  };
  GOptionContext *opt = opt_new(N_("<keyword> - search games"), entries);
  RhFilter f = { .multi = -1, .has_patch = -1, .sort = "name" };
  RhCtx *ctx;
  GPtrArray *games = NULL;
  RhErr e;
  int rc;

  rc = parse_common(opt, &a, _("usage: rom-hamster search <keyword>"), 2);
  if (rc) {
    g_option_context_free(opt);
    sub_args_free(&a);
    return rc;
  }
  f.kw = kw ? kw : a.argv[1];

  ctx = make_ctx();
  if (open_store_for(ctx, FALSE) != 0) {
    g_option_context_free(opt);
    sub_args_free(&a);
    rh_ctx_free(ctx);
    return 1;
  }
  e = rh_query_list(ctx, &f, &games);
  if (!rh_err_ok(&e)) {
    g_printerr(_("search failed: %s\n"), e.msg);
    rc = 1;
  } else {
    print_games(games);
    g_print(g_dngettext(GETTEXT_PACKAGE, "%u game\n", "%u games\n", games->len), games->len);
  }
  if (games)
    g_ptr_array_unref(games);
  store_close(ctx);
  g_option_context_free(opt);
  sub_args_free(&a);
  rh_ctx_free(ctx);
  return rc;
}

static const char *yn(gboolean b)
{
  return b ? _("yes") : _("no");
}

int cmd_show(int argc, char **argv)
{
  SubArgs a = sub_args_new(argc, argv);
  GOptionContext *opt = g_option_context_new(N_("<game-id> - show game detail"));
  RhCtx *ctx;
  RhGameDetail *d = NULL;
  RhErr e;
  int rc;

  g_option_context_set_help_enabled(opt, TRUE);
  g_option_context_set_translation_domain(opt, GETTEXT_PACKAGE);
  if (!g_option_context_parse(opt, &a.argc, &a.argv, NULL)) {
    g_option_context_free(opt);
    sub_args_free(&a);
    return 2;
  }
  if (a.argc < 2) {
    g_printerr(_("usage: rom-hamster show <game-id>\n"));
    g_option_context_free(opt);
    sub_args_free(&a);
    return 2;
  }

  ctx = make_ctx();
  if (open_store_for(ctx, FALSE) != 0) {
    g_option_context_free(opt);
    sub_args_free(&a);
    rh_ctx_free(ctx);
    return 1;
  }
  e = rh_detail_load(ctx, a.argv[1], &d);
  if (!rh_err_ok(&e)) {
    g_printerr(_("show failed: %s\n"), e.msg);
    rc = 1;
  } else {
    RhGame *g = d->game;
    g_print(_("id:          %s\n"), g->id);
    g_print(_("name:        %s\n"), g->name);
    if (g->game_main_name)
      g_print(_("main name:   %s\n"), g->game_main_name);
    g_print(_("platform:    %s\n"), g->platform);
    g_print(_("directory:   %s\n"), d->dir);
    g_print(_("developer:   %s\n"), g->developer ? g->developer : "");
    g_print(_("publisher:   %s\n"), g->publisher ? g->publisher : "");
    g_print(_("year:        %d\n"), g->year);
    g_print(_("genre:       %s\n"), g->genre ? g->genre : "");
    g_print(_("players:     %d\n"), g->players);
    if (g->rating >= 0)
      g_print(_("rating:      %.2f\n"), g->rating);
    else
      g_print(_("rating:      unset\n"));
    g_print(_("region:      %s\n"), g->region ? g->region : "");
    g_print(_("multi-disc:  %s\n"), yn(g->is_multi_disc));
    g_print(_("arcade:      %s\n"), yn(g->is_arcade));
    g_print(_("rom format:  %s\n"), g->rom_format ? g->rom_format : "");
    g_print(_("rom status:  %s\n"), g->rom_status ? g->rom_status : "");
    g_print(_("dat source:  %s\n"), g->dat_source ? g->dat_source : "");
    g_print(_("patch type:  %s\n"), g->patch_type ? g->patch_type : "");
    if (g->description && *g->description)
      g_print(_("description:\n%s\n"), g->description);

    for (int k = 0; k < RH_DETAIL_GROUP_COUNT; k++) {
      RhDetailGroup *grp = &d->groups[k];
      g_print(_("\n%s (%u):\n"), grp->kind, grp->files->len);
      for (guint i = 0; i < grp->files->len; i++) {
        RhDetailFile *df = g_ptr_array_index(grp->files, i);
        g_print("  %s\t%" G_GINT64_FORMAT "\t%s\n", df->filename, df->size,
                df->exists ? _("ok") : _("missing"));
      }
    }
  }
  rh_detail_free(d);
  store_close(ctx);
  rh_ctx_free(ctx);
  g_option_context_free(opt);
  sub_args_free(&a);
  return rc;
}

int cmd_edit(int argc, char **argv)
{
  const char *name = NULL, *genre = NULL, *developer = NULL;
  const char *publisher = NULL, *region = NULL, *status = NULL;
  const char *patch_type = NULL;
  gchar *description = NULL, *main_name = NULL;
  char **tags = NULL;
  int year = 0, players = 0;
  double rating = -1;
  gboolean clear_tags = FALSE;
  SubArgs a = sub_args_new(argc, argv);
  GOptionEntry entries[] = {
    { "name", 'n', 0, G_OPTION_ARG_STRING, &name,
      N_("Display name"), N_("TEXT") },
    { "main-name", 0, 0, G_OPTION_ARG_STRING, &main_name,
      N_("Main name"), N_("TEXT") },
    { "description", 0, 0, G_OPTION_ARG_STRING, &description,
      N_("Description"), N_("TEXT") },
    { "developer", 0, 0, G_OPTION_ARG_STRING, &developer,
      N_("Developer"), N_("TEXT") },
    { "publisher", 0, 0, G_OPTION_ARG_STRING, &publisher,
      N_("Publisher"), N_("TEXT") },
    { "genre", 0, 0, G_OPTION_ARG_STRING, &genre,
      N_("Genre"), N_("TEXT") },
    { "region", 0, 0, G_OPTION_ARG_STRING, &region,
      N_("Region"), N_("TEXT") },
    { "status", 0, 0, G_OPTION_ARG_STRING, &status,
      N_("ROM status"), N_("TEXT") },
    { "patch-type", 0, 0, G_OPTION_ARG_STRING, &patch_type,
      N_("Patch type"), N_("TEXT") },
    { "year", 0, 0, G_OPTION_ARG_INT, &year,
      N_("Release year"), N_("YEAR") },
    { "players", 0, 0, G_OPTION_ARG_INT, &players,
      N_("Players"), N_("N") },
    { "rating", 0, 0, G_OPTION_ARG_DOUBLE, &rating,
      N_("Rating 0.0-1.0"), N_("R") },
    { "tag", 't', 0, G_OPTION_ARG_STRING_ARRAY, &tags,
      N_("Tag (repeatable)"), N_("TAG") },
    { "clear-tags", 0, 0, G_OPTION_ARG_NONE, &clear_tags,
      N_("Clear all tags"), NULL },
    { NULL, 0, 0, 0, NULL, NULL, NULL },
  };
  GOptionContext *opt = opt_new(N_("<game-id> - edit metadata"), entries);
  RhCtx *ctx;
  RhGamePatch p = { 0 };
  RhErr e;
  int rc;

  rc = parse_common(opt, &a, _("usage: rom-hamster edit <game-id> [options]"), 2);
  if (rc) {
    g_option_context_free(opt);
    sub_args_free(&a);
    return rc;
  }
  if (clear_tags && tags) {
    g_printerr(_("--tag and --clear-tags are mutually exclusive\n"));
    g_option_context_free(opt);
    sub_args_free(&a);
    return 2;
  }

  p.name = name; p.set_name = name != NULL;
  p.game_main_name = main_name; p.set_game_main_name = main_name != NULL;
  p.description = description; p.set_description = description != NULL;
  p.developer = developer; p.set_developer = developer != NULL;
  p.publisher = publisher; p.set_publisher = publisher != NULL;
  p.genre = genre; p.set_genre = genre != NULL;
  p.region = region; p.set_region = region != NULL;
  p.rom_status = status; p.set_rom_status = status != NULL;
  p.patch_type = patch_type; p.set_patch_type = patch_type != NULL;
  p.year = year; p.set_year = year > 0;
  p.players = players; p.set_players = players > 0;
  p.rating = rating; p.set_rating = rating >= 0;
  p.tags = (const char **) tags;
  p.n_tags = tags ? (int) g_strv_length(tags) : 0;
  p.set_tags = tags != NULL || clear_tags;

  ctx = make_ctx();
  if (open_store_for(ctx, FALSE) != 0) {
    g_option_context_free(opt);
    sub_args_free(&a);
    rh_ctx_free(ctx);
    return 1;
  }
  e = rh_manage_update(ctx, a.argv[1], &p);
  if (!rh_err_ok(&e)) {
    g_printerr(_("edit failed: %s\n"), e.msg);
    rc = 1;
  } else {
    g_print(_("updated %s\n"), a.argv[1]);
  }
  store_close(ctx);
  rh_ctx_free(ctx);
  g_option_context_free(opt);
  sub_args_free(&a);
  return rc;
}

int cmd_delete(int argc, char **argv)
{
  gboolean files = FALSE, yes = FALSE;
  SubArgs a = sub_args_new(argc, argv);
  GOptionEntry entries[] = {
    { "files", 'f', 0, G_OPTION_ARG_NONE, &files,
      N_("Delete game directory too"), NULL },
    { "yes", 'y', 0, G_OPTION_ARG_NONE, &yes,
      N_("Confirm deletion"), NULL },
    { NULL, 0, 0, 0, NULL, NULL, NULL },
  };
  GOptionContext *opt = opt_new(N_("<game-id> - delete game"), entries);
  RhCtx *ctx;
  RhErr e;
  int rc;

  rc = parse_common(opt, &a, _("usage: rom-hamster delete <game-id> [-f] -y"), 2);
  if (rc) {
    g_option_context_free(opt);
    sub_args_free(&a);
    return rc;
  }
  if (!yes) {
    g_printerr(_("refusing to delete without --yes\n"));
    g_option_context_free(opt);
    sub_args_free(&a);
    return 2;
  }

  ctx = make_ctx();
  if (open_store_for(ctx, FALSE) != 0) {
    g_option_context_free(opt);
    sub_args_free(&a);
    rh_ctx_free(ctx);
    return 1;
  }
  e = rh_manage_delete(ctx, a.argv[1], files);
  if (!rh_err_ok(&e)) {
    g_printerr(_("delete failed: %s\n"), e.msg);
    rc = 1;
  } else if (files) {
    g_print(_("deleted %s (with files)\n"), a.argv[1]);
  } else {
    g_print(_("deleted %s\n"), a.argv[1]);
  }
  store_close(ctx);
  rh_ctx_free(ctx);
  g_option_context_free(opt);
  sub_args_free(&a);
  return rc;
}

int cmd_export(int argc, char **argv)
{
  const char *frontend = NULL;
  gboolean media = FALSE, covers = FALSE, screenshots = FALSE;
  gboolean videos = FALSE, marquees = FALSE, miximages = FALSE;
  gboolean intro = FALSE, patcher = FALSE, no_m3u = FALSE, compress = FALSE;
  char **platforms = NULL, **ids = NULL;
  SubArgs a = sub_args_new(argc, argv);
  GOptionEntry entries[] = {
    { "frontend", 'f', 0, G_OPTION_ARG_STRING, &frontend,
      N_("Frontend: es/esde/pegasus or path"), N_("ID") },
    { "platform", 'p', 0, G_OPTION_ARG_STRING_ARRAY, &platforms,
      N_("Platform (repeatable)"), N_("ID") },
    { "id", 0, 0, G_OPTION_ARG_STRING_ARRAY, &ids,
      N_("Game id (repeatable)"), N_("ID") },
    { "media", 'm', 0, G_OPTION_ARG_NONE, &media,
      N_("Export all media"), NULL },
    { "covers", 0, 0, G_OPTION_ARG_NONE, &covers,
      N_("Export covers"), NULL },
    { "screenshots", 0, 0, G_OPTION_ARG_NONE, &screenshots,
      N_("Export screenshots"), NULL },
    { "videos", 0, 0, G_OPTION_ARG_NONE, &videos,
      N_("Export videos"), NULL },
    { "marquees", 0, 0, G_OPTION_ARG_NONE, &marquees,
      N_("Export marquees"), NULL },
    { "miximages", 0, 0, G_OPTION_ARG_NONE, &miximages,
      N_("Export miximages"), NULL },
    { "intro", 0, 0, G_OPTION_ARG_NONE, &intro,
      N_("Export intro texts"), NULL },
    { "patcher", 0, 0, G_OPTION_ARG_NONE, &patcher,
      N_("Export patches"), NULL },
    { "no-m3u", 0, 0, G_OPTION_ARG_NONE, &no_m3u,
      N_("Do not generate m3u"), NULL },
    { "compress", 0, 0, G_OPTION_ARG_NONE, &compress,
      N_("Compressed ROM export (M3: unsupported)"), NULL },
    { NULL, 0, 0, 0, NULL, NULL, NULL },
  };
  GOptionContext *opt = opt_new(N_("<dest-dir> - export to a frontend"), entries);
  RhCtx *ctx;
  RhFrontend *fe = NULL;
  RhExportOpts o = { 0 };
  RhExportResult res = { 0 };
  GPtrArray *plats = NULL, *gids = NULL;
  gchar *fe_path = NULL;
  RhErr e;
  int rc;

  rc = parse_common(opt, &a,
                    _("usage: rom-hamster export <dest-dir> [--frontend es|esde|pegasus]"),
                    2);
  if (rc) {
    g_option_context_free(opt);
    sub_args_free(&a);
    return rc;
  }

  e = rh_frontend_find(frontend ? frontend : "es", &fe_path);
  if (!rh_err_ok(&e)) {
    g_printerr(_("frontend error: %s\n"), e.msg);
    g_option_context_free(opt);
    g_free((gpointer) frontend);
    g_strfreev(platforms);
    g_strfreev(ids);
    sub_args_free(&a);
    return 1;
  }
  e = rh_frontend_load(fe_path, &fe);
  g_free(fe_path);
  if (!rh_err_ok(&e)) {
    g_printerr(_("frontend error: %s\n"), e.msg);
    g_option_context_free(opt);
    g_free((gpointer) frontend);
    g_strfreev(platforms);
    g_strfreev(ids);
    sub_args_free(&a);
    return 1;
  }

  if (platforms) {
    plats = g_ptr_array_new();
    for (int i = 0; platforms[i]; i++)
      g_ptr_array_add(plats, platforms[i]);
  }
  if (ids) {
    gids = g_ptr_array_new();
    for (int i = 0; ids[i]; i++)
      g_ptr_array_add(gids, ids[i]);
  }

  o.dest_dir = a.argv[1];
  o.frontend = fe;
  o.platforms = plats;
  o.ids = gids;
  o.media_all = media;
  o.media_kinds[0] = covers;
  o.media_kinds[1] = screenshots;
  o.media_kinds[2] = videos;
  o.media_kinds[3] = marquees;
  o.media_kinds[4] = miximages;
  o.media_kinds[5] = intro;
  o.media_kinds[6] = patcher;
  o.write_roms = TRUE;
  o.generate_m3u = !no_m3u;
  o.compress = compress;

  ctx = make_ctx();
  if (open_store_for(ctx, FALSE) != 0) {
    rh_frontend_free(fe);
    g_option_context_free(opt);
    sub_args_free(&a);
    if (plats) g_ptr_array_unref(plats);
    if (gids) g_ptr_array_unref(gids);
    g_free((gpointer) frontend);
    g_strfreev(platforms);
    g_strfreev(ids);
    rh_ctx_free(ctx);
    return 1;
  }
  e = rh_export_run(ctx, &o, &res);
  if (!rh_err_ok(&e)) {
    g_printerr(_("export failed: %s\n"), e.msg);
    rc = 1;
  } else {
    char b_games[32], b_roms[32], b_media[32], b_skipped[32];

    g_print(_("exported %s games, %s roms, %s media; skipped %s -> %s\n"),
            i64str(res.games, b_games, sizeof b_games),
            i64str(res.roms, b_roms, sizeof b_roms),
            i64str(res.media, b_media, sizeof b_media),
            i64str(res.skipped, b_skipped, sizeof b_skipped),
            a.argv[1]);
  }
  store_close(ctx);
  rh_ctx_free(ctx);
  rh_frontend_free(fe);
  g_option_context_free(opt);
  if (plats)
    g_ptr_array_unref(plats);
  if (gids)
    g_ptr_array_unref(gids);
  g_free((gpointer) frontend);
  g_strfreev(platforms);
  g_strfreev(ids);
  sub_args_free(&a);
  return rc;
}

int cmd_stats(int argc, char **argv)
{
  SubArgs a = sub_args_new(argc, argv);
  GOptionContext *opt = g_option_context_new(N_("- library statistics"));
  RhCtx *ctx;
  RhStats st = { 0 };
  GPtrArray *by_plat = NULL;
  RhErr e;
  int rc = 0;

  g_option_context_set_help_enabled(opt, TRUE);
  g_option_context_set_translation_domain(opt, GETTEXT_PACKAGE);
  if (!g_option_context_parse(opt, &a.argc, &a.argv, NULL)) {
    g_option_context_free(opt);
    sub_args_free(&a);
    return 2;
  }
  ctx = make_ctx();
  if (open_store_for(ctx, FALSE) != 0) {
    g_option_context_free(opt);
    sub_args_free(&a);
    rh_ctx_free(ctx);
    return 1;
  }
  e = rh_query_stats(ctx, &st, &by_plat);
  if (!rh_err_ok(&e)) {
    g_printerr(_("stats failed: %s\n"), e.msg);
    rc = 1;
  } else {
    char b_games[32], b_platforms[32], b_roms[32];
    char b_media_files[32], b_media_bytes[32], b_dat_entries[32];

    g_print(_("games:       %s\n"),
            i64str(st.games, b_games, sizeof b_games));
    g_print(_("platforms:   %s\n"),
            i64str(st.platforms, b_platforms, sizeof b_platforms));
    g_print(_("roms:        %s\n"),
            i64str(st.roms, b_roms, sizeof b_roms));
    g_print(_("media files: %s\n"),
            i64str(st.media_files, b_media_files, sizeof b_media_files));
    g_print(_("media bytes: %s\n"),
            i64str(st.media_bytes, b_media_bytes, sizeof b_media_bytes));
    g_print(_("dat entries: %s\n"),
            i64str(st.dat_entries, b_dat_entries, sizeof b_dat_entries));
    g_print(_("\nplatform\tgames\tmedia bytes\n"));
    for (guint i = 0; by_plat && i < by_plat->len; i++) {
      RhPlatformStat *p = g_ptr_array_index(by_plat, i);
      g_print("%s\t%" G_GINT64_FORMAT "\t%" G_GINT64_FORMAT "\n",
              p->platform, p->games, p->media_bytes);
    }
  }
  if (by_plat)
    g_ptr_array_unref(by_plat);
  store_close(ctx);
  rh_ctx_free(ctx);
  g_option_context_free(opt);
  sub_args_free(&a);
  return rc;
}
