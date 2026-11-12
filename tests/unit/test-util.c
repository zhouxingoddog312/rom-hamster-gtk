/*
 * test-util.c — rh-str / rh-fs / rh-hash 单测
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "rh-err.h"
#include "rh-archive.h"
#include "rh-fs.h"
#include "rh-hash.h"
#include "rh-str.h"

static void test_sanitize(void)
{
  gchar *s, *t;
  gint64 n;
  GString *big = g_string_new(NULL);

  s = rh_sanitize_dirname("a/b\\c:d*e?f\"g<h>i|j");
  g_assert_cmpstr(s, ==, "a_b_c_d_e_f_g_h_i_j");
  g_free(s);

  /* 制表符属控制字符（0x09），先被删除，再折叠空格——与 1.0.0 的
   * tr -d '\000-\037' 之后才 s/[[:space:]]+/ /g 的顺序一致 */
  s = rh_sanitize_dirname("  Name   with\t\tspaces  ");
  g_assert_cmpstr(s, ==, "Name withspaces");
  g_free(s);

  /* 控制字符丢弃 */
  s = rh_sanitize_dirname("ab\x01\x02" "cd");
  g_assert_cmpstr(s, ==, "abcd");
  g_free(s);

  /* 截断到 180 字符 */
  for (n = 0; n < 250; n++)
    g_string_append_c(big, 'x');
  t = rh_sanitize_dirname(big->str);
  g_assert_cmpint((gint64) g_utf8_strlen(t, -1), ==, 180);
  g_free(t);
  g_string_free(big, TRUE);
}

static void test_ext(void)
{
  gchar *s;

  s = rh_ext_of("/roms/Game.NES");
  g_assert_cmpstr(s, ==, ".nes");
  g_free(s);

  s = rh_ext_of("noext");
  g_assert_cmpstr(s, ==, "");
  g_free(s);

  s = rh_base_noext("/roms/My.Game.v2.zip");
  g_assert_cmpstr(s, ==, "My.Game.v2");
  g_free(s);

  s = rh_rom_base_name("/roms/cart.p8.png", ".p8.png");
  g_assert_cmpstr(s, ==, "cart");
  g_free(s);

  s = rh_rom_base_name("/roms/Game.nes", ".nes");
  g_assert_cmpstr(s, ==, "Game");
  g_free(s);
}

static void test_disc(void)
{
  gchar *main = NULL;

  g_assert_cmpint(rh_parse_disc_no("Game (Disc 2)", &main), ==, 2);
  g_assert_cmpstr(main, ==, "Game");
  g_free(main);

  g_assert_cmpint(rh_parse_disc_no("Game (CD1)", &main), ==, 1);
  g_assert_cmpstr(main, ==, "Game");
  g_free(main);

  g_assert_cmpint(rh_parse_disc_no("Game (Side B)", &main), ==, 2);
  g_assert_cmpstr(main, ==, "Game");
  g_free(main);

  g_assert_cmpint(rh_parse_disc_no("Game Side A", &main), ==, 1);
  g_free(main);

  g_assert_cmpint(rh_parse_disc_no("Plain Game", &main), ==, 0);
  g_assert_cmpstr(main, ==, "Plain Game");
  g_free(main);
}

static void test_sim(void)
{
  g_assert_cmpint(rh_name_sim("Sonic", "Sonic"), ==, 100);
  g_assert_cmpint(rh_name_sim("", ""), ==, 100);
  g_assert_cmpint(rh_name_sim("Sonic the Hedgehog", "Sonic The Hedgehog 2") > 50, ==, TRUE);
  g_assert_cmpint(rh_name_sim("abc", "abcdef"), ==, 66);
  g_assert_cmpint(rh_name_sim("mario", "zelda"), <, 30);
}

static void test_hash(void)
{
  gchar *dir = g_dir_make_tmp("rh-util-XXXXXX", NULL);
  gchar *path, *dest;
  RhErr err;
  RhHash h;
  int rc;

  g_assert_nonnull(dir);
  path = g_build_filename(dir, "hello.txt", NULL);
  g_assert_true(g_file_set_contents(path, "hello", 5, NULL));

  rh_err_clear(&err);
  rc = rh_hash_file(path, TRUE, TRUE, &h, &err);
  g_assert_cmpint(rc, ==, 0);
  g_assert_cmpint(h.size, ==, 5);
  g_assert_cmpstr(h.crc32, ==, "3610a686");
  g_assert_cmpstr(h.md5, ==, "5d41402abc4b2a76b9719d911017c592");
  g_assert_cmpstr(h.sha1, ==, "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d");
  rh_hash_clear(&h);

  /* 原子复制：首次 0，再次同 size+mtime 为 2 */
  dest = g_build_filename(dir, "copy.txt", NULL);
  rh_err_clear(&err);
  g_assert_cmpint(rh_fs_copy_atomic(path, dest, FALSE, &err), ==, 0);
  g_assert_true(rh_fs_is_file(dest));
  g_assert_cmpint(rh_fs_copy_atomic(path, dest, FALSE, &err), ==, 2);

  /* 不覆盖时返回 3 */
  g_assert_true(g_file_set_contents(dest, "different", 9, NULL));
  g_assert_cmpint(rh_fs_copy_atomic(path, dest, FALSE, &err), ==, 3);
  g_assert_cmpint(rh_fs_copy_atomic(path, dest, TRUE, &err), ==, 0);

  g_remove(dest);
  g_remove(path);
  g_rmdir(dir);
  g_free(dest);
  g_free(path);
  g_free(dir);
}

static void test_sniff(void)
{
  gchar *dir = g_dir_make_tmp("rh-sniff-XXXXXX", NULL);
  gchar *png, *txt, *bin;
  const guchar magic[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0};

  g_assert_nonnull(dir);
  png = g_build_filename(dir, "fake.jpg", NULL);
  txt = g_build_filename(dir, "who.md", NULL);
  bin = g_build_filename(dir, "rom.bin", NULL);

  g_file_set_contents(png, (const gchar *) magic, sizeof magic, NULL);
  g_file_set_contents(txt, "# Title\n\nText body\n", -1, NULL);
  g_file_set_contents(bin, "\x00\x01\x02\x03random", 10, NULL);

  g_assert_true(rh_fs_is_image(png));       /* 按内容，不按扩展名 */
  g_assert_true(rh_fs_is_text(txt));
  g_assert_false(rh_fs_is_text(bin));

  g_remove(png);
  g_remove(txt);
  g_remove(bin);
  g_rmdir(dir);
  g_free(png);
  g_free(txt);
  g_free(bin);
  g_free(dir);
}

static void test_cancel(void)
{
  gchar *dir = g_dir_make_tmp("rh-cancel-XXXXXX", NULL);
  gchar *src, *dest, *zip, *outdir;
  GString *body = g_string_sized_new(512 * 1024);
  RhErr e = rh_ok();
  gint cancel = 1;
  int rc;

  g_assert_nonnull(dir);
  src = g_build_filename(dir, "src.bin", NULL);
  dest = g_build_filename(dir, "dest.bin", NULL);
  zip = g_build_filename(dir, "pack.zip", NULL);
  outdir = g_build_filename(dir, "out", NULL);
  for (guint i = 0; i < 512 * 1024; i++)
    g_string_append_c(body, (char) (i & 0xff));
  g_file_set_contents(src, body->str, (gssize) body->len, NULL);
  g_string_free(body, TRUE);

  rc = rh_fs_copy_atomic_cancellable(src, dest, FALSE, &cancel, &e);
  g_assert_cmpint(rc, ==, 4);
  g_assert_false(g_file_test(dest, G_FILE_TEST_EXISTS));
  g_assert_cmpint(e.code, ==, RH_ERR_CANCELLED);

  {
    GPtrArray *files = g_ptr_array_new();

    g_ptr_array_add(files, src);
    g_assert_cmpint(rh_arc_create_zip(zip, files, &e), ==, 0);
    g_ptr_array_unref(files);
  }
  cancel = 1;
  rc = rh_arc_extract_cancellable(zip, outdir, NULL, &cancel, &e);
  g_assert_cmpint(rc, ==, 2);
  g_assert_false(g_file_test(g_build_filename(outdir, "src.bin", NULL),
                             G_FILE_TEST_EXISTS));

  g_remove(src);
  g_remove(dest);
  g_remove(zip);
  g_rmdir(outdir);
  g_rmdir(dir);
  g_free(src);
  g_free(dest);
  g_free(zip);
  g_free(outdir);
  g_free(dir);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/util/sanitize", test_sanitize);
  g_test_add_func("/util/ext", test_ext);
  g_test_add_func("/util/disc", test_disc);
  g_test_add_func("/util/sim", test_sim);
  g_test_add_func("/util/hash", test_hash);
  g_test_add_func("/util/sniff", test_sniff);
  g_test_add_func("/util/cancel", test_cancel);
  return g_test_run();
}
