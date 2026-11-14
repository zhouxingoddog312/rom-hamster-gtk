/*
 * test-config.c — 配置分层单测（M0 验收的一部分）
 */
#include <glib.h>
#include <glib/gstdio.h>

#include "rh-config.h"

static gchar *tmpdir;

static gchar *write_conf(const char *name, const char *body)
{
  gchar *path = g_build_filename(tmpdir, name, NULL);
  GError *err = NULL;

  g_assert_true(g_file_set_contents(path, body, -1, &err));
  g_assert_no_error(err);
  return path;
}

/* 后加载的层覆盖先加载的层，且记录来源行号 */
static void test_precedence(void)
{
  RhConfig *c = rh_config_new();
  gchar *low = write_conf("low.conf", "[default]\nROM_ROOT = /srv/roms\n");
  gchar *high = write_conf("high.conf", "# comment\n[default]\n"
                                         "ROM_ROOT = /home/u/roms\n");
  RhErr e;

  e = rh_config_load_file(c, RH_LAYER_SYSTEM, low);
  g_assert_true(rh_err_ok(&e));
  g_assert_cmpstr(rh_config_get(c, "ROM_ROOT"), ==, "/srv/roms");

  e = rh_config_load_file(c, RH_LAYER_USER, high);
  g_assert_true(rh_err_ok(&e));
  g_assert_cmpstr(rh_config_get(c, "ROM_ROOT"), ==, "/home/u/roms");

  const RhConfigValue *v = rh_config_get_value(c, "ROM_ROOT");
  g_assert_cmpint(v->layer, ==, RH_LAYER_USER);
  g_assert_cmpint(v->line, ==, 3);          /* 段头后第 3 行 */
  g_assert_cmpstr(v->source_file, ==, high);
  g_assert_cmpstr(v->shadowed_value, ==, "/srv/roms");

  gchar *text = rh_config_explain(c, "ROM_ROOT");
  g_assert_nonnull(g_strstr_len(text, -1, "overrides /srv/roms"));

  g_free(text);
  g_free(low);
  g_free(high);
  rh_config_free(c);
}

/* 缺文件不是错误：每一层都可能不存在 */
static void test_missing_file(void)
{
  RhConfig *c = rh_config_new();
  gchar *path = g_build_filename(tmpdir, "nope.conf", NULL);
  RhErr e = rh_config_load_file(c, RH_LAYER_FACTORY, path);

  g_assert_true(rh_err_ok(&e));
  g_assert_cmpstr(rh_config_get(c, "types"),
                  ==,
                  NULL);   /* 未定义的键返回 NULL，不猜 */

  g_free(path);
  rh_config_free(c);
}

/* 非 [default] 段的键被忽略；内置默认值来自编译期 */
static void test_groups_and_defaults(void)
{
  RhConfig *c = rh_config_new();
  gchar *p = write_conf("groups.conf",
                        "[other]\nROM_ROOT = /wrong\n[default]\n"
                        "duplicate_check_threshold = 85\n");
  RhErr e = rh_config_load_file(c, RH_LAYER_USER, p);

  g_assert_true(rh_err_ok(&e));
  g_assert_cmpstr(rh_config_get(c, "ROM_ROOT"), ==, "");
  g_assert_cmpint(rh_config_get_int(c, "duplicate_check_threshold", 70), ==, 85);
  g_assert_cmpint(rh_config_get_int(c, "dat_auto_match_multi_disc", 0), ==, 0);
  g_assert_true(rh_config_get_bool(c, "auto_generate_m3u"));

  g_free(p);
  rh_config_free(c);
}

/* 1.0.0 的 shell 风格配置（KEY="value"）必须能直接读：引号不算值的一部分 */
static void test_quoted_values(void)
{
  RhConfig *c = rh_config_new();
  gchar *p = write_conf("quoted.conf",
                        "[default]\n"
                        "ROM_ROOT=\"\"\n"
                        "dat_dir='/srv/dats'\n"
                        "dat_match_hash = \"crc32\"\n"
                        "disc_label_prefix = \"Disc\"\n"
                        "emulator_launch = \"retroarch -L core.so\"\n"
                        "auto_delete_source = true\n"
                        "store_path = \"unterminated\n");
  RhErr e = rh_config_load_file(c, RH_LAYER_SYSTEM, p);

  g_assert_true(rh_err_ok(&e));
  g_assert_cmpstr(rh_config_get(c, "ROM_ROOT"), ==, "");
  g_assert_cmpstr(rh_config_get(c, "dat_dir"), ==, "/srv/dats");
  g_assert_cmpstr(rh_config_get(c, "dat_match_hash"), ==, "crc32");
  g_assert_cmpstr(rh_config_get(c, "disc_label_prefix"), ==, "Disc");
  g_assert_cmpstr(rh_config_get(c, "emulator_launch"), ==, "retroarch -L core.so");
  g_assert_true(rh_config_get_bool(c, "auto_delete_source"));
  /* 引号不成对时按字面值保留，不做猜测 */
  g_assert_cmpstr(rh_config_get(c, "store_path"), ==, "\"unterminated");

  g_free(p);
  rh_config_free(c);
}

/* 语法错误必须报出文件名与行号，不能静默 */
static void test_parse_error(void)
{
  RhConfig *c = rh_config_new();
  gchar *p = write_conf("broken.conf", "[default]\nthis line has no equals\n");
  RhErr e = rh_config_load_file(c, RH_LAYER_USER, p);

  g_assert_false(rh_err_ok(&e));
  g_assert_cmpint(e.code, ==, RH_ERR_PARSE);
  g_assert_nonnull(g_strstr_len(e.msg, -1, "broken.conf:2"));

  g_free(p);
  rh_config_free(c);
}

static void cleanup_tmpdir(void)
{
  GDir *d = g_dir_open(tmpdir, 0, NULL);
  const gchar *name;

  if (d) {
    while ((name = g_dir_read_name(d)) != NULL) {
      gchar *p = g_build_filename(tmpdir, name, NULL);
      g_remove(p);
      g_free(p);
    }
    g_dir_close(d);
  }
  g_rmdir(tmpdir);
  g_free(tmpdir);
}


/* GUI 首次运行保存 ROM_ROOT：替换已有键、保留其他行与注释 */
static void test_save_user_key(void)
{
  gchar *dir = g_build_filename(tmpdir, "rom-hamster", NULL);
  gchar *path = g_build_filename(dir, "rom-hamster.conf", NULL);
  gchar *body;
  RhErr e;

  g_mkdir_with_parents(dir, 0755);
  g_assert_true(g_file_set_contents(path, "dat_dir=/roms/dat\n"
                                          "ROM_ROOT = /old\n"
                                          "# keep me\n", -1, NULL));

  e = rh_config_save_user_key("ROM_ROOT", "/new/root");
  g_assert_true(rh_err_ok(&e));
  g_assert_true(g_file_get_contents(path, &body, NULL, NULL));
  g_assert_nonnull(strstr(body, "dat_dir=/roms/dat"));
  g_assert_nonnull(strstr(body, "ROM_ROOT=/new/root"));
  g_assert_null(strstr(body, "/old"));
  g_assert_nonnull(strstr(body, "# keep me"));
  g_assert_cmpint(g_strv_length(g_strsplit(body, "ROM_ROOT=", -1)), ==, 2);

  g_free(body);
  g_free(path);
  g_free(dir);
}

int main(int argc, char **argv)
{
  int rc;

  g_test_init(&argc, &argv, NULL);
  tmpdir = g_dir_make_tmp("rh-cfg-XXXXXX", NULL);
  g_assert_nonnull(tmpdir);
  g_setenv("XDG_CONFIG_HOME", tmpdir, TRUE);

  g_test_add_func("/config/precedence", test_precedence);
  g_test_add_func("/config/missing-file", test_missing_file);
  g_test_add_func("/config/groups-defaults", test_groups_and_defaults);
  g_test_add_func("/config/quoted-values", test_quoted_values);
  g_test_add_func("/config/parse-error", test_parse_error);
  g_test_add_func("/config/save-user-key", test_save_user_key);

  rc = g_test_run();
  cleanup_tmpdir();
  return rc;
}
