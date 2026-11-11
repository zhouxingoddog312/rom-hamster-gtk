/*
 * test-platform.c — platforms.conf 五段解析与判定（M1 验收）
 *
 * 用法：test-platform <data/platforms.conf>
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "platform.h"
#include "rh-err.h"

static const char *data_path;

static RhPlatformRules *load_main(void)
{
  RhPlatformRules *r = platform_rules_new();
  RhErr e = platform_rules_load(r, data_path);

  g_assert_true(rh_err_ok(&e));
  return r;
}

static gboolean platform_has_ext(RhPlatform *p, const char *ext)
{
  for (guint i = 0; i < p->extensions->len; i++) {
    if (g_strcmp0(g_ptr_array_index(p->extensions, i), ext) == 0)
      return TRUE;
  }
  return FALSE;
}

static gboolean platform_has_alias(RhPlatform *p, const char *alias)
{
  for (guint i = 0; i < p->aliases->len; i++) {
    if (g_strcmp0(g_ptr_array_index(p->aliases, i), alias) == 0)
      return TRUE;
  }
  return FALSE;
}

/* ------------------------------------------------------------ 段 1：[platform_extensions] */

static void test_platform_extensions(void)
{
  RhPlatformRules *r = load_main();

  g_assert_cmpuint(platform_count(r), ==, 101);

  RhPlatform *nes = platform_get(r, "nes");
  g_assert_nonnull(nes);
  g_assert_true(platform_has_ext(nes, ".nes"));
  g_assert_true(platform_has_ext(nes, ".unf"));
  g_assert_true(nes->compress_supported);
  g_assert_cmpstr(nes->recommended_format, ==, "zip");
  g_assert_false(nes->arcade);
  g_assert_cmpuint(nes->patch_formats->len, >, 0);
  g_assert_cmpstr(nes->source_file, !=, NULL);
  g_assert_cmpint(nes->line, >, 0);

  /* 相对 1.0.0 的四处修正，逐条钉住 */
  RhPlatform *pce = platform_get(r, "pcengine");
  g_assert_nonnull(pce);
  g_assert_false(platform_has_ext(pce, ".tg16"));

  RhPlatform *videopac = platform_get(r, "videopac");
  g_assert_nonnull(videopac);
  g_assert_false(platform_has_ext(videopac, ".jpg"));
  g_assert_true(platform_has_ext(videopac, ".bin"));

  RhPlatform *megaduck = platform_get(r, "megaduck");
  g_assert_nonnull(megaduck);
  g_assert_true(platform_has_ext(megaduck, ".bin"));
  g_assert_false(platform_has_ext(megaduck, ".md"));

  RhPlatform *c16 = platform_get(r, "c16");
  RhPlatform *plus4 = platform_get(r, "plus4");
  g_assert_nonnull(c16);
  g_assert_nonnull(plus4);
  g_assert_false(platform_has_alias(c16, "plus4"));
  g_assert_true(platform_has_alias(plus4, "plus4"));

  /* 街机基板标记必须解析进来 */
  gboolean arcade_seen = FALSE;
  for (guint i = 0; i < platform_all(r)->len; i++) {
    RhPlatform *p = g_ptr_array_index(platform_all(r), i);
    if (p->arcade)
      arcade_seen = TRUE;
  }
  g_assert_true(arcade_seen);

  platform_rules_free(r);
}

/* ------------------------------------------------------------ 段 2-5 */

static void test_other_sections(void)
{
  RhPlatformRules *r = load_main();

  /* [arcade_rom_suffixes] */
  g_assert_true(platform_is_arcade_rom_suffix(r, ".rom"));
  g_assert_true(platform_is_arcade_rom_suffix(r, ".chr"));
  g_assert_false(platform_is_arcade_rom_suffix(r, ".iso"));

  /* [archive_extensions] */
  g_assert_true(platform_is_archive(r, ".zip"));
  g_assert_true(platform_is_archive(r, ".7z"));
  g_assert_true(platform_is_archive(r, ".rar"));
  g_assert_true(platform_is_archive(r, "ZIP"));   /* 大小写不敏感 */
  g_assert_false(platform_is_archive(r, ".nes"));

  /* [media_type_dirs] */
  g_assert_cmpstr(platform_media_dir(r, "covers"), ==, "covers");
  g_assert_cmpstr(platform_media_dir(r, "rom"), ==, "rom");
  g_assert_cmpstr(platform_media_dir(r, "unknown-kind"), ==, "unknown-kind");

  /* [media_suffix_rules]：顺序决定 .png/.jpg 的归属 */
  g_assert_cmpstr(platform_media_kind(r, "Box.jpg"), ==, "covers");
  g_assert_cmpstr(platform_media_kind(r, "shot.png"), ==, "covers");
  g_assert_cmpstr(platform_media_kind(r, "clip.mp4"), ==, "videos");
  g_assert_cmpstr(platform_media_kind(r, "readme.txt"), ==, "intro");
  g_assert_cmpstr(platform_media_kind(r, "fix.ips"), ==, "patcher");
  g_assert_null(platform_media_kind(r, "rom.nes"));       /* ROM 不是媒体 */
  g_assert_null(platform_media_kind(r, "weird.xyz"));
  g_assert_null(platform_media_kind(r, "noext"));

  platform_rules_free(r);
}

static void test_extension_lookup(void)
{
  RhPlatformRules *r = load_main();
  GPtrArray *arr;
  gboolean  seen_nes = FALSE;

  arr = platform_by_extension(r, ".nes");
  g_assert_nonnull(arr);
  g_assert_cmpuint(arr->len, >=, 1);
  for (guint i = 0; i < arr->len; i++) {
    if (g_strcmp0(((RhPlatform *) g_ptr_array_index(arr, i))->id, "nes") == 0)
      seen_nes = TRUE;
  }
  g_assert_true(seen_nes);

  /* .bin 是二义扩展名：必须命中多个平台，交给 ui->choose 裁决 */
  arr = platform_by_extension(r, ".bin");
  g_assert_nonnull(arr);
  g_assert_cmpuint(arr->len, >, 1);

  arr = platform_candidates_for_file(r, "/roms/foo.sfc");
  g_assert_nonnull(arr);
  g_assert_cmpuint(arr->len, >=, 1);

  /* 目录名反查：别名与 "平台 - 主机" 两种写法 */
  RhPlatform *p = platform_resolve_dir(r, "famicom");
  g_assert_nonnull(p);
  g_assert_cmpstr(p->id, ==, "nes");

  p = platform_resolve_dir(r, "Nintendo - NES");
  g_assert_nonnull(p);
  g_assert_cmpstr(p->id, ==, "nes");

  g_assert_null(platform_resolve_dir(r, "definitely-not-a-platform"));

  platform_rules_free(r);
}

/* ------------------------------------------------------------ 覆盖与报错 */

static void test_override_and_errors(void)
{
  RhPlatformRules *r = platform_rules_new();
  RhErr e = platform_rules_load(r, data_path);
  gchar *tmpdir, *over_path, *bad_path, *sec_path;

  g_assert_true(rh_err_ok(&e));

  tmpdir = g_dir_make_tmp("rh-plat-XXXXXX", NULL);
  g_assert_nonnull(tmpdir);

  /* 后一层重新定义 nes：必须整条替换，而不是叠加扩展名 */
  over_path = g_build_filename(tmpdir, "override.conf", NULL);
  g_file_set_contents(over_path,
    "[platform_extensions]\n"
    "nes:.nes;compress_supported=true;recommended_format=zip;arcade=false\n",
    -1, NULL);
  e = platform_rules_load(r, over_path);
  g_assert_true(rh_err_ok(&e));

  RhPlatform *nes = platform_get(r, "nes");
  g_assert_nonnull(nes);
  g_assert_cmpuint(nes->extensions->len, ==, 1);
  g_assert_cmpstr(nes->source_file, ==, over_path);

  /* 语法错误必须报出 文件:行 */
  bad_path = g_build_filename(tmpdir, "bad.conf", NULL);
  g_file_set_contents(bad_path, "[platform_extensions]\nbadline\n", -1, NULL);
  RhPlatformRules *r2 = platform_rules_new();
  e = platform_rules_load(r2, bad_path);
  g_assert_false(rh_err_ok(&e));
  g_assert_cmpint(e.code, ==, RH_ERR_PARSE);
  g_assert_nonnull(strstr(e.msg, ":2"));
  platform_rules_free(r2);

  /* 未知段名同样在加载期报错，不静默忽略 */
  sec_path = g_build_filename(tmpdir, "sec.conf", NULL);
  g_file_set_contents(sec_path, "[nosuchsection]\nkey=value\n", -1, NULL);
  RhPlatformRules *r3 = platform_rules_new();
  e = platform_rules_load(r3, sec_path);
  g_assert_false(rh_err_ok(&e));
  g_assert_nonnull(strstr(e.msg, "unknown section"));
  platform_rules_free(r3);

  g_remove(bad_path);
  g_remove(sec_path);
  g_remove(over_path);
  g_rmdir(tmpdir);
  g_free(bad_path);
  g_free(sec_path);
  g_free(over_path);
  g_free(tmpdir);

  platform_rules_free(r);
}

/* ------------------------------------------------------------ 缺文件 */

static void test_missing_file(void)
{
  RhPlatformRules *r = platform_rules_new();
  RhErr e = platform_rules_load(r, "/nonexistent/platforms.conf");

  g_assert_true(rh_err_ok(&e));       /* 该层不覆盖，不是错误 */
  g_assert_cmpuint(platform_count(r), ==, 0);
  platform_rules_free(r);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  if (argc < 2) {
    g_printerr("usage: %s <data/platforms.conf>\n", argv[0]);
    return 77;
  }
  data_path = argv[1];

  g_test_add_func("/platform/extensions", test_platform_extensions);
  g_test_add_func("/platform/other-sections", test_other_sections);
  g_test_add_func("/platform/extension-lookup", test_extension_lookup);
  g_test_add_func("/platform/override-and-errors", test_override_and_errors);
  g_test_add_func("/platform/missing-file", test_missing_file);
  return g_test_run();
}
