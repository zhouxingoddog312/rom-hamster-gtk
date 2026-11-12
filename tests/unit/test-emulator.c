/*
 * test-emulator.c — emulators.conf 解析、模板展开与全局兜底
 */
#include <glib.h>
#include <glib/gstdio.h>

#include "emulator.h"
#include "rh-config.h"
#include "rh-ctx.h"

static void test_parse_and_expand(void)
{
  RhCtx *ctx = rh_ctx_new();
  RhEmuRules *r = rh_emu_rules_new();
  gchar *dir = g_dir_make_tmp("rh-emu-XXXXXX", NULL);
  gchar *path = g_build_filename(dir, "emulators.conf", NULL);
  gchar *rom = g_build_filename(dir, "My Game.nes", NULL);
  gchar *game_dir = g_build_filename(dir, "My Game", NULL);
  gchar **argv = NULL;
  gchar *command = NULL;
  int argc = 0;
  RhErr e;

  g_file_set_contents(path,
                      "# comment\n"
                      "nes: /bin/echo \"%ROM%\" \"%DIR%\"\n"
                      "snes = /bin/echo \"{file}\" \"{dir}\"\n"
                      "gb: /bin/echo \"%ROM%\"\n"
                      "gbc: /bin/echo\n",
                      -1, NULL);

  e = rh_emu_rules_load_file(r, path);
  g_assert_cmpint(e.code, ==, RH_OK);
  g_assert_cmpstr(rh_emu_rules_lookup(r, "nes"), ==,
                  "/bin/echo \"%ROM%\" \"%DIR%\"");

  e = rh_emu_command_build(ctx, r, "nes", rom, game_dir, &argv, &command);
  g_assert_cmpint(e.code, ==, RH_OK);
  g_assert_cmpstr(argv[0], ==, "/bin/echo");
  g_assert_cmpstr(argv[1], ==, rom);
  g_assert_cmpstr(argv[2], ==, game_dir);
  {
    gchar *want = g_strdup_printf("/bin/echo \"%s\" \"%s\"", rom, game_dir);
    g_assert_cmpstr(command, ==, want);
    g_free(want);
  }
  g_free(command);
  g_strfreev(argv);
  argv = NULL;
  command = NULL;

  /* 兼容 {file} / {dir} */
  e = rh_emu_command_build(ctx, r, "snes", rom, game_dir, &argv, &command);
  g_assert_cmpint(e.code, ==, RH_OK);
  g_assert_cmpstr(argv[1], ==, rom);
  g_assert_cmpstr(argv[2], ==, game_dir);
  g_free(command);
  g_strfreev(argv);
  argv = NULL;

  /* 路径里带双引号也不能被 g_shell_parse_argv 拆开。 */
  {
    gchar *rom_q = g_build_filename(dir, "Q\"uote Game.nes", NULL);

    e = rh_emu_command_build(ctx, r, "nes", rom_q, game_dir, &argv, &command);
    g_assert_cmpint(e.code, ==, RH_OK);
    g_assert_cmpstr(argv[1], ==, rom_q);
    g_free(command);
    g_strfreev(argv);
    argv = NULL;
    g_free(rom_q);
  }

  /* 平台没有单独配置时退回 emulator_launch */
  rh_config_set(ctx->config, "emulator_launch", "/bin/echo \"%ROM%\"",
                RH_LAYER_USER);
  e = rh_emu_command_build(ctx, r, "md", rom, game_dir, &argv, &command);
  g_assert_cmpint(e.code, ==, RH_OK);
  g_assert_cmpstr(argv[1], ==, rom);
  g_free(command);
  g_strfreev(argv);
  argv = NULL;

  /* 没有 %ROM% / {file} 时明确失败，不偷偷追加参数 */
  e = rh_emu_command_build(ctx, r, "gbc", rom, game_dir, &argv, &command);
  g_assert_cmpint(e.code, ==, RH_ERR_CONFIG);
  g_assert_nonnull(g_strstr_len(e.msg, -1, "no %ROM%"));

  g_free(path);
  g_free(rom);
  g_free(game_dir);
  g_rmdir(dir);
  g_free(dir);
  rh_emu_rules_free(r);
  rh_ctx_free(ctx);
}

static void test_parse_error_line(void)
{
  RhEmuRules *r = rh_emu_rules_new();
  gchar *dir = g_dir_make_tmp("rh-emu-bad-XXXXXX", NULL);
  gchar *path = g_build_filename(dir, "emulators.conf", NULL);
  RhErr e;

  g_file_set_contents(path, "nes: ok\n\nbroken-line\n", -1, NULL);
  e = rh_emu_rules_load_file(r, path);
  g_assert_cmpint(e.code, ==, RH_ERR_PARSE);
  g_assert_nonnull(g_strstr_len(e.msg, -1, ":3:"));
  g_assert_nonnull(g_strstr_len(e.msg, -1, "platform: command"));

  g_free(path);
  g_rmdir(dir);
  g_free(dir);
  rh_emu_rules_free(r);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/emulator/parse-expand", test_parse_and_expand);
  g_test_add_func("/emulator/parse-error", test_parse_error_line);
  return g_test_run();
}
