/*
 * ui-cli.c — rh_ui 的终端实现
 *
 * M0 只实现"能回答的问题"：log / progress / confirm / choose / enter /
 * text_view。文件选择在纯 CLI 下没有意义（用户自己给路径），M2 的
 * pick_* 会返回 NULL 让调用方走参数路径。
 */
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "rh-ui.h"

static const char *level_tag(RhLogLevel lv)
{
  switch (lv) {
  case RH_LOG_DEBUG: return "debug";
  case RH_LOG_INFO:  return "info";
  case RH_LOG_WARN:  return "warning";
  case RH_LOG_ERROR: return "error";
  default:           return "log";
  }
}

static void cli_log(void *ud, RhLogLevel lv, const char *msg)
{
  (void) ud;
  g_printerr("%s: %s\n", level_tag(lv), msg);
}

static void cli_progress(void *ud, double pct, const char *label)
{
  (void) ud;
  if (pct < 0)
    g_printerr("\r%s ...", label ? label : "working");
  else
    g_printerr("\r%s %.0f%%", label ? label : "working", pct * 100.0);
  fflush(stderr);
}

static int cli_confirm(void *ud, const char *title, const char *msg,
                       int default_yes)
{
  char buf[8];
  (void) ud;

  g_printerr("%s\n%s [%s]: ", title, msg, default_yes ? "Y/n" : "y/N");
  if (!fgets(buf, sizeof buf, stdin))
    return default_yes;
  buf[strcspn(buf, "\n")] = '\0';
  if (buf[0] == '\0')
    return default_yes;
  return buf[0] == 'y' || buf[0] == 'Y';
}

static int cli_choose(void *ud, const char *title, const char **items, int n,
                      const char *remember_key)
{
  char buf[32];
  int  sel;
  (void) ud;

  g_printerr("%s\n", title);
  for (int i = 0; i < n; i++)
    g_printerr("  %d) %s\n", i + 1, items[i]);
  g_printerr("select [1-%d, empty = cancel]: ", n);

  if (!fgets(buf, sizeof buf, stdin))
    return -1;
  if (sscanf(buf, "%d", &sel) != 1)
    return -1;
  if (sel < 1 || sel > n)
    return -1;
  (void) remember_key;   /* CLI 的"应用到全部"由调用方栈上变量承担 */
  return sel - 1;
}

static char *cli_enter(void *ud, const char *title, const char *label,
                       const char *def)
{
  char  buf[512];
  (void) ud;

  g_printerr("%s — %s%s%s: ", title, label ? label : "",
             def && *def ? " [" : "", def && *def ? def : "");
  if (def && *def)
    g_printerr("]");
  if (!fgets(buf, sizeof buf, stdin))
    return NULL;
  buf[strcspn(buf, "\n")] = '\0';
  if (buf[0] == '\0')
    return def ? g_strdup(def) : NULL;
  return g_strdup(buf);
}

static char *cli_password(void *ud, const char *title, const char *label)
{
  struct termios old, noecho;
  char buf[512];
  (void) ud;

  g_printerr("%s — %s: ", title, label ? label : "password");
  if (tcgetattr(STDIN_FILENO, &old) != 0)
    return NULL;
  noecho = old;
  noecho.c_lflag &= ~(tcflag_t) ECHO;
  tcsetattr(STDIN_FILENO, TCSANOW, &noecho);

  char *ret = fgets(buf, sizeof buf, stdin);
  tcsetattr(STDIN_FILENO, TCSANOW, &old);
  g_printerr("\n");
  if (!ret)
    return NULL;
  buf[strcspn(buf, "\n")] = '\0';
  return g_strdup(buf);
}

static GPtrArray *cli_pick_files(void *ud, const char *title, int multiple)
{
  (void) ud; (void) title; (void) multiple;
  return NULL;   /* 纯 CLI 由用户直接给路径参数 */
}

static char *cli_pick_dir(void *ud, const char *title)
{
  (void) ud; (void) title;
  return NULL;
}

static void cli_text_view(void *ud, const char *title, const char *text)
{
  (void) ud;
  g_print("%s\n%s\n", title, text);
}

static int cli_ask_media_kind(void *ud, const char *path,
                              const char *remember_key)
{
  (void) ud; (void) path; (void) remember_key;
  return -1;   /* M2 再补：按 media_type_dirs 猜，猜不中才问 */
}

static const RhUi cli_ui = {
  .userdata        = NULL,
  .log             = cli_log,
  .progress        = cli_progress,
  .confirm         = cli_confirm,
  .choose          = cli_choose,
  .enter           = cli_enter,
  .password        = cli_password,
  .pick_files      = cli_pick_files,
  .pick_dir        = cli_pick_dir,
  .text_view       = cli_text_view,
  .ask_media_kind  = cli_ask_media_kind,
};

const RhUi *ui_cli_get(void)
{
  return &cli_ui;
}
