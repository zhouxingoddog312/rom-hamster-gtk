/*
 * emulator.c — emulators.conf 解析、模板展开与 spawn（ARCHITECTURE §7.1）
 *
 * 核心库不打印文字；失败只返回 RhErr，由 CLI/GUI 决定怎么显示。
 */
#include "emulator.h"

#include <string.h>

#include "rh-config.h"
#include "rh-dirs.h"

struct RhEmuRules {
  GHashTable *by_platform;   /* platform -> command template */
};

RhEmuRules *rh_emu_rules_new(void)
{
  RhEmuRules *r = g_new0(RhEmuRules, 1);

  r->by_platform = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        g_free);
  return r;
}

void rh_emu_rules_free(RhEmuRules *r)
{
  if (!r)
    return;
  g_hash_table_destroy(r->by_platform);
  g_free(r);
}

const gchar *rh_emu_rules_lookup(RhEmuRules *r, const char *platform)
{
  if (!r || !platform || !*platform)
    return NULL;
  return g_hash_table_lookup(r->by_platform, platform);
}

RhErr rh_emu_rules_load_file(RhEmuRules *r, const char *path)
{
  gchar  *content = NULL;
  gchar **lines;
  GError *gerr = NULL;
  RhErr   e = rh_ok();

  if (!r || !path)
    return rh_fail(RH_ERR_CONFIG, "emulator: bad arguments");
  if (!g_file_get_contents(path, &content, NULL, &gerr)) {
    /* 文件不存在 = 该层不覆盖；其他读取错误必须报出来。 */
    if (g_error_matches(gerr, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_error_free(gerr);
      return rh_ok();
    }
    e = rh_fail(RH_ERR_IO, "emulator: cannot read %s: %s", path,
                gerr->message);
    g_error_free(gerr);
    return e;
  }

  lines = g_strsplit(content, "\n", -1);
  for (int i = 0; lines[i]; i++) {
    gchar *line = g_strstrip(lines[i]);
    gchar *sep, *platform, *command;

    if (*line == '\0' || *line == '#' || *line == ';')
      continue;
    sep = strchr(line, ':');
    if (!sep)
      sep = strchr(line, '=');
    if (!sep) {
      e = rh_fail(RH_ERR_PARSE, "%s:%d: expected platform: command", path,
                  i + 1);
      goto out;
    }
    *sep = '\0';
    platform = g_strstrip(line);
    command = g_strstrip(sep + 1);
    if (!*platform || !*command) {
      e = rh_fail(RH_ERR_PARSE, "%s:%d: empty platform or command", path,
                  i + 1);
      goto out;
    }
    g_hash_table_replace(r->by_platform, g_strdup(platform),
                         g_strdup(command));
  }

out:
  g_strfreev(lines);
  g_free(content);
  return e;
}

RhErr rh_emu_rules_load_standard(RhEmuRules *r)
{
  gchar *user_path;
  RhErr  e;

  e = rh_emu_rules_load_file(r, RH_DATADIR "/emulators.conf");
  if (!rh_err_ok(&e))
    return e;
  e = rh_emu_rules_load_file(r, RH_SYSCONFDIR "/emulators.conf");
  if (!rh_err_ok(&e))
    return e;
  user_path = g_build_filename(g_get_user_config_dir(), "rom-hamster",
                               "emulators.conf", NULL);
  e = rh_emu_rules_load_file(r, user_path);
  g_free(user_path);
  return e;
}

/* 展开 %ROM% / {file} / %DIR% / {dir}；返回是否出现过 ROM 占位符。 */
static gchar *expand_template(const char *tmpl, const char *rom,
                              const char *dir, gboolean *has_rom)
{
  static const struct {
    const char *token;
    const char *value;
    gboolean    is_rom;
  } tokens[] = {
    { "%ROM%",  NULL, TRUE  },
    { "{file}", NULL, TRUE  },
    { "%DIR%",  NULL, FALSE },
    { "{dir}",  NULL, FALSE },
  };
  GString *out = g_string_new(NULL);
  const char *p = tmpl;

  *has_rom = FALSE;
  while (*p) {
    const char *best = NULL;
    guint best_i = 0;

    for (guint i = 0; i < G_N_ELEMENTS(tokens); i++) {
      const char *hit = strstr(p, tokens[i].token);

      if (hit && (!best || hit < best)) {
        best = hit;
        best_i = i;
      }
    }
    if (!best) {
      g_string_append(out, p);
      break;
    }
    g_string_append_len(out, p, best - p);
    if (best_i == 0)
      g_string_append(out, rom);
    else if (best_i == 1)
      g_string_append(out, rom);
    else
      g_string_append(out, dir ? dir : "");
    if (tokens[best_i].is_rom)
      *has_rom = TRUE;
    p = best + strlen(tokens[best_i].token);
  }
  return g_string_free(out, FALSE);
}

/* 在已解析的 argv 里把标记替换成真实路径；避免路径里的引号/空格破坏参数切分。 */
static gchar *replace_marker(const gchar *s, const gchar *marker,
                             const gchar *value)
{
  GString *out = g_string_new(NULL);
  const gchar *p = s;

  while (p && *p) {
    const gchar *hit = strstr(p, marker);

    if (!hit) {
      g_string_append(out, p);
      break;
    }
    g_string_append_len(out, p, hit - p);
    g_string_append(out, value ? value : "");
    p = hit + strlen(marker);
  }
  return g_string_free(out, FALSE);
}

RhErr rh_emu_command_build(RhCtx *ctx, RhEmuRules *rules, const char *platform,
                           const char *rom_path, const char *game_dir,
                           gchar ***argv_out, gchar **command_out)
{
  const gchar *tmpl = NULL;
  gchar *expanded = NULL;
  gchar *parse_str = NULL;
  gchar **argv = NULL;
  GError *gerr = NULL;
  gboolean has_rom = FALSE;
  int argc = 0;
  RhErr e = rh_ok();

  if (argv_out)
    *argv_out = NULL;
  if (command_out)
    *command_out = NULL;
  if (!argv_out || !platform || !*platform || !rom_path || !*rom_path)
    return rh_fail(RH_ERR_CONFIG, "emulator: missing platform or ROM");

  if (rules)
    tmpl = rh_emu_rules_lookup(rules, platform);
  if (!tmpl || !*tmpl)
    tmpl = ctx ? rh_config_get(ctx->config, "emulator_launch") : NULL;
  if (!tmpl || !*tmpl)
    return rh_fail(RH_ERR_CONFIG,
                   "no emulator command for platform '%s'; configure "
                   "~/.config/rom-hamster/emulators.conf or emulator_launch",
                   platform);

  expanded = expand_template(tmpl, rom_path, game_dir, &has_rom);
  parse_str = expand_template(tmpl, "@@RH_ROM@@", "@@RH_DIR@@", &has_rom);
  if (!has_rom) {
    e = rh_fail(RH_ERR_CONFIG,
                "emulator template for '%s' has no %%ROM%% or {file}",
                platform);
    goto out;
  }
  if (!g_shell_parse_argv(parse_str, &argc, &argv, &gerr)) {
    e = rh_fail(RH_ERR_PARSE, "emulator command parse failed: %s",
                gerr ? gerr->message : "unknown error");
    g_clear_error(&gerr);
    goto out;
  }
  if (!argv || !argv[0] || !*argv[0]) {
    e = rh_fail(RH_ERR_CONFIG, "emulator command is empty");
    goto out;
  }
  for (int i = 0; i < argc; i++) {
    gchar *arg = replace_marker(argv[i], "@@RH_ROM@@", rom_path);

    g_free(argv[i]);
    argv[i] = replace_marker(arg, "@@RH_DIR@@", game_dir);
    g_free(arg);
  }
  if (command_out)
    *command_out = g_strdup(expanded);
  *argv_out = argv;
  argv = NULL;

out:
  g_strfreev(argv);
  g_free(expanded);
  g_free(parse_str);
  return e;
}

gchar *rh_emu_pick_rom(const RhGameDetail *d)
{
  const RhDetailGroup *roms;

  if (!d || !d->groups[0].files)
    return NULL;
  roms = &d->groups[0];

  /* 多碟优先由导入生成的 m3u；没有 m3u 才落到第一个存在的 ROM。 */
  for (guint i = 0; i < roms->files->len; i++) {
    RhDetailFile *f = g_ptr_array_index(roms->files, i);
    const gchar *dot = f->filename ? strrchr(f->filename, '.') : NULL;

    if (f->exists && dot && g_ascii_strcasecmp(dot, ".m3u") == 0)
      return g_strdup(f->path);
  }
  for (guint i = 0; i < roms->files->len; i++) {
    RhDetailFile *f = g_ptr_array_index(roms->files, i);

    if (f->exists)
      return g_strdup(f->path);
  }
  return NULL;
}

static void emu_child_exit(GPid pid, gint status, gpointer data)
{
  (void) status;
  (void) data;
  g_spawn_close_pid(pid);
}

RhErr rh_emu_launch(RhCtx *ctx, const RhGameDetail *d, gchar **rom_out)
{
  RhEmuRules *rules = NULL;
  gchar *rom = NULL, *command = NULL;
  gchar **argv = NULL;
  GError *gerr = NULL;
  GPid pid = 0;
  RhErr e;

  if (rom_out)
    *rom_out = NULL;
  if (!ctx || !d)
    return rh_fail(RH_ERR_CONFIG, "emulator: bad arguments");

  rom = rh_emu_pick_rom(d);
  if (!rom)
    return rh_fail(RH_ERR_NOT_FOUND, "emulator: no existing ROM to launch");

  rules = rh_emu_rules_new();
  e = rh_emu_rules_load_standard(rules);
  if (!rh_err_ok(&e))
    goto out;

  e = rh_emu_command_build(ctx, rules, d->game ? d->game->platform : NULL,
                           rom, d->dir, &argv, &command);
  if (!rh_err_ok(&e))
    goto out;

  if (!g_spawn_async(d->dir, argv, NULL,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                     NULL, NULL, &pid, &gerr)) {
    e = rh_fail(RH_ERR_IO, "emulator spawn failed: %s",
                gerr ? gerr->message : "unknown error");
    g_clear_error(&gerr);
    goto out;
  }
  /* GUI 要长时间运行，必须回收子进程，避免僵尸。 */
  g_child_watch_add(pid, emu_child_exit, NULL);
  if (rom_out)
    *rom_out = g_strdup(rom);

out:
  g_free(rom);
  g_free(command);
  g_strfreev(argv);
  rh_emu_rules_free(rules);
  return e;
}
