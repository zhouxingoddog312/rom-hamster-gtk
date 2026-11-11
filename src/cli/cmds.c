/*
 * 子命令：init / import / dat
 *
 * GOptionContext 把 argv[0] 当程序名，因此子命令参数要先补一个占位 argv[0]。
 */
#include "cmds.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "dat.h"
#include "import.h"
#include "platform.h"
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

static RhCtx *make_ctx(void)
{
  RhCtx *ctx = rh_ctx_new();

  rh_ctx_set_ui(ctx, ui_cli_get());
  return ctx;
}

static int open_store_for(RhCtx *ctx, gboolean create)
{
  const gchar *root = rh_config_get(ctx->config, "ROM_ROOT");
  gchar *dbp = rh_ctx_resolve_db_path(ctx, root);
  RhErr e;

  if (!dbp) {
    g_printerr("ROM_ROOT 未配置: 先运行 rom-hamster init --root <dir>\n");
    return 1;
  }
  e = store_open(ctx, dbp, create ? STORE_OPEN_CREATE : 0);
  g_free(dbp);
  if (!rh_err_ok(&e)) {
    g_printerr("打开数据库失败: %s\n", e.msg);
    return 1;
  }
  return 0;
}

int cmd_init(int argc, char **argv)
{
  const char *root = NULL, *dat_dir = NULL;
  GOptionEntry entries[] = {
    { "root", 0, 0, G_OPTION_ARG_STRING, &root, "ROM root directory", "<dir>" },
    { "dat-dir", 0, 0, G_OPTION_ARG_STRING, &dat_dir, "DAT directory", "<dir>" },
    { NULL, 0, 0, 0, NULL, NULL, NULL },
  };
  GOptionContext *opt = g_option_context_new("- initialize a ROM library");
  GError *gerr = NULL;
  SubArgs a = sub_args_new(argc, argv);
  RhCtx *ctx;
  gchar *user_path;
  int rc = 0;

  g_option_context_add_main_entries(opt, entries, NULL);
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
    g_printerr("usage: rom-hamster init --root <dir> [--dat-dir <dir>]\n");
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
        g_printerr("写配置失败: %s (%s)\n", user_path, e.msg);
        rc = 1;
      }
      g_free(content);
    }

    if (rc == 0) {
      if (open_store_for(ctx, TRUE) == 0) {
        g_print("ROM_ROOT: %s\n", canon);
        g_print("store:    %s\n", ctx->db_path);
        g_print("config:   %s\n", user_path);
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
    { "yes", 'y', 0, G_OPTION_ARG_NONE, &yes, "non-interactive defaults", NULL },
    { "dry-run", 0, 0, G_OPTION_ARG_NONE, &dry, "scan only", NULL },
    { "overwrite", 0, 0, G_OPTION_ARG_NONE, &overwrite, "overwrite differing files", NULL },
    { "root", 0, 0, G_OPTION_ARG_STRING, &root, "override ROM_ROOT", "<dir>" },
    { "platforms", 0, 0, G_OPTION_ARG_STRING, &platforms, "extra platform rules", "<file>" },
    { NULL, 0, 0, 0, NULL, NULL, NULL },
  };
  GOptionContext *opt = g_option_context_new("<paths> - import ROMs and media");
  GError *gerr = NULL;
  SubArgs a = sub_args_new(argc, argv);
  RhCtx *ctx;
  RhPlatformRules *rules;
  GPtrArray *inputs;
  RhImportReport *rep = NULL;
  RhErr e = rh_ok();
  int rc = 0;

  g_option_context_add_main_entries(opt, entries, NULL);
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
    g_printerr("平台规则加载失败: %s\n", e.msg);
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
    g_printerr("at least one file or directory is required\n");
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
      g_printerr("导入失败: %s\n", e.msg);
      rc = 1;
    }
  }
  if (rep) {
    g_print("planned %d: imported %d, media %d, skipped %d, failed %d (job %" G_GINT64_FORMAT ")\n",
            rep->planned, rep->imported, rep->media, rep->skipped, rep->failed,
            rep->job_id);
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
    g_printerr("usage: rom-hamster dat import <platform> <file.dat>\n");
    return 2;
  }
  ctx = make_ctx();
  if (open_store_for(ctx, TRUE) != 0) {
    rh_ctx_free(ctx);
    return 1;
  }
  e = rh_dat_import_file(ctx, argv[1], argv[2], NULL);
  if (!rh_err_ok(&e)) {
    g_printerr("DAT 导入失败: %s\n", e.msg);
    rc = 1;
  } else {
    g_print("DAT indexed: %s -> %s\n", argv[2], argv[1]);
  }
  store_close(ctx);
  rh_ctx_free(ctx);
  return rc;
}
