/*
 * rom-hamster CLI（M0 骨架）
 *
 * 核心库只给常量，所有面向用户的文字都在这一层（ARCHITECTURE §1.1）。
 * 文案按 §11 约定用英文 msgid，gettext 接入放到 M5。
 */
#include <glib.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

#include "cmds.h"
#include "rh-config.h"
#include "rh-ui.h"
#include "rh-version.h"

extern const RhUi *ui_cli_get(void);   /* ui-cli.c */

static void print_version(void)
{
  g_print("%s %s\n", RH_APP_NAME, RH_VERSION);
  g_print("maintainer: %s <%s>\n", RH_MAINTAINER, RH_MAINTAINER_EMAIL);
  g_print("homepage: %s\n", RH_HOMEPAGE);
  g_print("license: %s\n", RH_LICENSE);
  g_print("schema: v%d\n", RH_SCHEMA_VERSION);
}

static int cmd_config(RhConfig *cfg, int argc, char **argv)
{
  if (argc >= 2 && g_strcmp0(argv[0], "--explain") == 0) {
    const RhConfigValue *v = rh_config_get_value(cfg, argv[1]);
    gchar *text = rh_config_explain(cfg, argv[1]);

    if (!v) {
      g_printerr("%s", text);
      g_free(text);
      return 1;
    }
    g_print("%s", text);
    g_free(text);
    return 0;
  }

  if (argc >= 1 && g_strcmp0(argv[0], "--list") == 0) {
    guint   n = 0;
    gchar **keys = rh_config_keys(cfg, &n);

    for (guint i = 0; i < n; i++) {
      const RhConfigValue *v = rh_config_get_value(cfg, keys[i]);
      g_print("%-32s = %s  [%s]\n", keys[i], v->value,
              rh_layer_name(v->layer));
    }
    g_strfreev(keys);
    return 0;
  }

  g_printerr("usage: rom-hamster config --explain <KEY> | --list\n");
  return 2;
}

int main(int argc, char **argv)
{
  setlocale(LC_ALL, "");
  gboolean show_version = FALSE;
  const char *cmd = NULL;
  gchar **rest = NULL;
  int restc = 0;

  GOptionEntry entries[] = {
    { "version", 'V', 0, G_OPTION_ARG_NONE, &show_version,
      "Print version and maintainer information", NULL },
    { NULL, 0, 0, 0, NULL, NULL, NULL },
  };

  GOptionContext *opt = g_option_context_new("<command> — ROM collection manager");
  GError *gerr = NULL;

  g_option_context_set_help_enabled(opt, TRUE);
  g_option_context_add_main_entries(opt, entries, NULL);
  /* 遇到第一个非选项就停下，把子命令参数原样留给子命令 */
  g_option_context_set_strict_posix(opt, TRUE);

  /* 只解析全局选项，子命令参数留给各自处理 */
  if (!g_option_context_parse(opt, &argc, &argv, &gerr)) {
    g_printerr("%s\n", gerr->message);
    g_error_free(gerr);
    g_option_context_free(opt);
    return 2;
  }
  g_option_context_free(opt);

  if (show_version) {
    print_version();
    return 0;
  }

  if (argc < 2) {
    g_printerr("usage: rom-hamster <command>  (try --help)\n");
    return 2;
  }

  cmd  = argv[1];
  rest = argv + 2;
  restc = argc - 2;

  RhConfig *cfg = rh_config_new();
  rh_config_load_standard(cfg);

  int rc;
  if (g_strcmp0(cmd, "config") == 0) {
    rc = cmd_config(cfg, restc, rest);
  } else if (g_strcmp0(cmd, "init") == 0) {
    rc = cmd_init(restc, rest);
  } else if (g_strcmp0(cmd, "import") == 0) {
    rc = cmd_import(restc, rest);
  } else if (g_strcmp0(cmd, "dat") == 0) {
    rc = cmd_dat(restc, rest);
  } else if (g_strcmp0(cmd, "list") == 0) {
    rc = cmd_list(restc, rest);
  } else if (g_strcmp0(cmd, "search") == 0) {
    rc = cmd_search(restc, rest);
  } else if (g_strcmp0(cmd, "show") == 0) {
    rc = cmd_show(restc, rest);
  } else if (g_strcmp0(cmd, "edit") == 0) {
    rc = cmd_edit(restc, rest);
  } else if (g_strcmp0(cmd, "delete") == 0) {
    rc = cmd_delete(restc, rest);
  } else if (g_strcmp0(cmd, "export") == 0) {
    rc = cmd_export(restc, rest);
  } else if (g_strcmp0(cmd, "stats") == 0) {
    rc = cmd_stats(restc, rest);
  } else {
    g_printerr("unknown command: %s\n", cmd);
    rc = 2;
  }

  rh_config_free(cfg);
  return rc;
}
