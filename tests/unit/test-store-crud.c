/*
 * test-store-crud.c — store_* 的 CRUD、约束、级联与作业/DAT（M1 验收）
 *
 * 全部通过 store_* API 访问，测试里不写 SQL（与 §5.1 硬规则一致）。
 * 每个用例开一个独立的小库，避免用例之间互相污染计数。
 */
#include <glib.h>
#include <glib/gstdio.h>

#include "rh-ctx.h"
#include "rh-err.h"
#include "rh-records.h"
#include "store.h"

static gchar *tmpdir;
static RhCtx *ctx;

/* ---------------------------------------------------------------- 助手 */

static RhCtx *open_ctx(const char *name)
{
  gchar *dir = g_build_filename(tmpdir, name, NULL);
  gchar *path = g_build_filename(dir, "romhamster.db", NULL);
  RhCtx *c = rh_ctx_new();
  RhErr  e = store_open(c, path, STORE_OPEN_CREATE);

  if (!rh_err_ok(&e)) {
    g_printerr("store_open(%s) failed: %s\n", name, e.msg);
    g_assert_true(rh_err_ok(&e));
  }
  g_free(path);
  g_free(dir);
  ctx = c;
  return c;
}

static void close_ctx(void)
{
  store_close(ctx);
  rh_ctx_free(ctx);
  ctx = NULL;
}

static RhGame *mk_game(const char *id, const char *platform, const char *name,
                       const char *dir)
{
  RhGame *g = rh_game_new();

  g->id = g_strdup(id);
  g->platform = g_strdup(platform);
  g->name = g_strdup(name);
  g->dir_path = g_strdup(dir);
  g_free(g->rom_format);
  g_free(g->rom_status);
  g->rom_format = g_strdup("zip");
  g->rom_status = g_strdup("good");
  return g;
}

static void assert_ok(RhErr e, const char *what)
{
  if (!rh_err_ok(&e)) {
    g_printerr("%s failed: %s\n", what, e.msg);
    g_assert_true(rh_err_ok(&e));
  }
}

static guint list_len(GPtrArray *a)
{
  return a ? a->len : 0;
}

/* ---------------------------------------------------------------- CRUD */

static void test_crud_roundtrip(void)
{
  RhGame *g, *got = NULL;
  gchar *created;

  open_ctx("crud");

  g = mk_game("g1", "nes", "Super Mario Bros", "nes/Super Mario Bros");
  g->year = 1985;
  g->players = 2;
  g->rating = 0.85;
  g->developer = g_strdup("Nintendo");
  g->description = g_strdup("platformer");

  assert_ok(store_game_upsert(ctx, g), "upsert");
  assert_ok(store_game_get(ctx, "g1", &got), "get");
  g_assert_cmpstr(got->platform, ==, "nes");
  g_assert_cmpstr(got->name, ==, "Super Mario Bros");
  g_assert_cmpint(got->year, ==, 1985);
  g_assert_cmpint(got->players, ==, 2);
  g_assert_cmpfloat(got->rating, ==, 0.85);
  g_assert_cmpstr(got->rom_status, ==, "good");
  g_assert_nonnull(got->created_at);
  created = g_strdup(got->created_at);
  rh_game_free(got);

  /* 再 upsert（不带 created_at）：名称更新，created_at 必须保留 */
  g_free(g->name);
  g->name = g_strdup("Super Mario Bros (Rev A)");
  g->rating = -1.0;
  assert_ok(store_game_upsert(ctx, g), "re-upsert");

  got = NULL;
  assert_ok(store_game_get(ctx, "g1", &got), "get after update");
  g_assert_cmpstr(got->name, ==, "Super Mario Bros (Rev A)");
  g_assert_cmpstr(got->created_at, ==, created);
  g_assert_cmpfloat(got->rating, ==, -1.0);      /* NULL → 哨兵 */
  rh_game_free(got);
  g_free(created);
  rh_game_free(g);

  /* 未知字段的哨兵：year 0、rating -1 也要原样回来 */
  g = mk_game("g2", "snes", "Chrono Trigger", "snes/Chrono Trigger");
  assert_ok(store_game_upsert(ctx, g), "upsert g2");
  got = NULL;
  assert_ok(store_game_get(ctx, "g2", &got), "get g2");
  g_assert_cmpint(got->year, ==, 0);
  g_assert_cmpfloat(got->rating, ==, -1.0);
  rh_game_free(got);
  rh_game_free(g);

  close_ctx();
}

/* id 为空时按 dir_path 生成：同一目录重复 upsert 只留一条 */
static void test_auto_id(void)
{
  const char *dir = "gb/Tetris";
  gchar *expect = rh_game_gen_id("gb", dir);
  RhGame *g, *got = NULL;
  gint64 n = -1;

  open_ctx("auto-id");

  g = mk_game(NULL, "gb", "Tetris", dir);
  assert_ok(store_game_upsert(ctx, g), "upsert auto id 1");
  rh_game_free(g);

  g = mk_game(NULL, "gb", "Tetris (Rev A)", dir);
  assert_ok(store_game_upsert(ctx, g), "upsert auto id 2 (same dir)");
  rh_game_free(g);

  assert_ok(store_game_get(ctx, expect, &got), "get by generated id");
  g_assert_cmpstr(got->name, ==, "Tetris (Rev A)");
  rh_game_free(got);

  assert_ok(store_game_count(ctx, &n), "count");
  g_assert_cmpint(n, ==, 1);          /* 同一 dir_path 只占一行 */
  g_free(expect);

  close_ctx();
}

static void test_dir_path_unique(void)
{
  RhGame *a, *b;
  RhErr e;

  open_ctx("unique");

  a = mk_game("u1", "nes", "A", "nes/shared-dir");
  b = mk_game("u2", "nes", "B", "nes/shared-dir");

  assert_ok(store_game_upsert(ctx, a), "upsert a");
  e = store_game_upsert(ctx, b);
  g_assert_false(rh_err_ok(&e));
  g_assert_cmpint(e.code, ==, RH_ERR_EXISTS);

  rh_game_free(a);
  rh_game_free(b);
  close_ctx();
}

/* ---------------------------------------------------------------- 子表与级联 */

static void test_children_and_cascade(void)
{
  RhGame *g = mk_game("c1", "psx", "Final Fantasy VII", "psx/Final Fantasy VII");
  RhRomFile *r1, *r2;
  GPtrArray *list;
  const char *tags[] = { "  RPG ", "Action", "Action", "   " };
  gint64 total = 0;
  RhErr e;

  open_ctx("cascade");

  assert_ok(store_game_upsert(ctx, g), "upsert c1");

  r1 = rh_rom_file_new();
  r1->filename = g_strdup("Disc 1.bin");
  r1->disc_no = 1;
  r1->size = 700000000;
  r1->crc32 = g_strdup("DEADBEEF");
  assert_ok(store_rom_add(ctx, "c1", r1), "rom add 1");

  r2 = rh_rom_file_new();
  r2->filename = g_strdup("Disc 2.bin");
  r2->disc_no = 2;
  assert_ok(store_rom_add(ctx, "c1", r2), "rom add 2");

  assert_ok(store_rom_add(ctx, "c1", r1), "rom add idempotent");

  list = NULL;
  assert_ok(store_rom_list(ctx, "c1", &list), "rom list");
  g_assert_cmpuint(list_len(list), ==, 2);
  g_assert_cmpint(((RhRomFile *) g_ptr_array_index(list, 0))->disc_no, ==, 1);
  g_assert_cmpint(((RhRomFile *) g_ptr_array_index(list, 1))->disc_no, ==, 2);
  g_ptr_array_unref(list);

  assert_ok(store_media_add_sized(ctx, "c1", "covers", "covers/front.png", 1024, 0),
            "media cover");
  assert_ok(store_media_add_sized(ctx, "c1", "screenshots",
                                  "screenshots/1.png", 2048, 1), "media shot");
  assert_ok(store_media_add(ctx, "c1", "videos", "videos/opening.mp4"),
            "media video (unknown size)");

  list = NULL;
  assert_ok(store_media_list(ctx, "c1", "covers", &list), "media list covers");
  g_assert_cmpuint(list_len(list), ==, 1);
  g_assert_cmpint(((RhMediaFile *) g_ptr_array_index(list, 0))->size, ==, 1024);
  g_ptr_array_unref(list);

  list = NULL;
  assert_ok(store_media_list(ctx, "c1", NULL, &list), "media list all");
  g_assert_cmpuint(list_len(list), ==, 3);
  g_ptr_array_unref(list);

  assert_ok(store_media_total_size(ctx, "c1", &total), "media total");
  g_assert_cmpint(total, ==, 1024 + 2048);    /* NULL size 不参与 SUM */

  assert_ok(store_tags_set(ctx, "c1", tags, 4), "tags set");
  list = NULL;
  assert_ok(store_game_tags(ctx, "c1", &list), "game tags");
  g_assert_cmpuint(list_len(list), ==, 2);    /* 去重 + 空白项丢弃 */
  g_ptr_array_unref(list);

  /* 级联删除：games 一删，roms / media / game_tags 全部消失 */
  assert_ok(store_game_delete(ctx, "c1"), "delete game");

  list = NULL;
  assert_ok(store_rom_list(ctx, "c1", &list), "rom list after delete");
  g_assert_cmpuint(list_len(list), ==, 0);
  g_ptr_array_unref(list);

  list = NULL;
  assert_ok(store_media_list(ctx, "c1", NULL, &list), "media list after delete");
  g_assert_cmpuint(list_len(list), ==, 0);
  g_ptr_array_unref(list);

  list = NULL;
  assert_ok(store_game_tags(ctx, "c1", &list), "tags after delete");
  g_assert_cmpuint(list_len(list), ==, 0);
  g_ptr_array_unref(list);

  RhGame *gone = NULL;
  e = store_game_get(ctx, "c1", &gone);
  g_assert_false(rh_err_ok(&e));
  g_assert_cmpint(e.code, ==, RH_ERR_NOT_FOUND);

  /* 删不存在的记录要报 NOT_FOUND，而不是静默成功 */
  e = store_game_delete(ctx, "c1");
  g_assert_false(rh_err_ok(&e));
  g_assert_cmpint(e.code, ==, RH_ERR_NOT_FOUND);

  rh_rom_file_free(r1);
  rh_rom_file_free(r2);
  rh_game_free(g);
  close_ctx();
}

/* ---------------------------------------------------------------- 筛选 */

static void test_filters(void)
{
  RhGame *g;
  GPtrArray *list;
  RhFilter *f;
  const char *tags[] = { "classic" };
  RhErr e;
  gint64 n = -1;

  open_ctx("filters");

  g = mk_game("f1", "nes", "Super Mario Bros", "nes/f1");
  g->year = 1985;
  g->rating = 0.9;
  assert_ok(store_game_upsert(ctx, g), "f1");
  rh_game_free(g);

  g = mk_game("f2", "nes", "Super Mario Bros 3", "nes/f2");
  g->year = 1988;
  g->rating = 0.95;
  assert_ok(store_game_upsert(ctx, g), "f2");
  rh_game_free(g);

  g = mk_game("f3", "snes", "超级马里奥世界", "snes/f3");
  g->year = 1990;
  assert_ok(store_game_upsert(ctx, g), "f3");
  rh_game_free(g);

  g = mk_game("f4", "psx", "Multi Disc Game", "psx/f4");
  g->is_multi_disc = TRUE;
  g_free(g->patch_type);
  g->patch_type = g_strdup("generic");
  assert_ok(store_game_upsert(ctx, g), "f4");
  rh_game_free(g);

  assert_ok(store_tags_set(ctx, "f1", tags, 1), "tag f1");

  /* 平台 */
  f = rh_filter_new();
  f->platform = "nes";
  list = NULL;
  assert_ok(store_game_list(ctx, f, &list), "filter platform");
  g_assert_cmpuint(list_len(list), ==, 2);
  g_ptr_array_unref(list);
  rh_filter_free(f);

  /* 年份区间 */
  f = rh_filter_new();
  f->year_from = 1988;
  list = NULL;
  assert_ok(store_game_list(ctx, f, &list), "filter year");
  g_assert_cmpuint(list_len(list), ==, 2);     /* 1988 + 1990 */
  g_ptr_array_unref(list);
  rh_filter_free(f);

  /* 多碟 */
  f = rh_filter_new();
  f->multi = 1;
  list = NULL;
  assert_ok(store_game_list(ctx, f, &list), "filter multi");
  g_assert_cmpuint(list_len(list), ==, 1);
  g_assert_cmpstr(((RhGame *) g_ptr_array_index(list, 0))->id, ==, "f4");
  g_ptr_array_unref(list);
  rh_filter_free(f);

  /* 有补丁 */
  f = rh_filter_new();
  f->has_patch = 1;
  list = NULL;
  assert_ok(store_game_list(ctx, f, &list), "filter has_patch");
  g_assert_cmpuint(list_len(list), ==, 1);
  g_ptr_array_unref(list);
  rh_filter_free(f);

  /* 标签 */
  f = rh_filter_new();
  f->tags = tags;
  f->n_tags = 1;
  list = NULL;
  assert_ok(store_game_list(ctx, f, &list), "filter tag");
  g_assert_cmpuint(list_len(list), ==, 1);
  g_assert_cmpstr(((RhGame *) g_ptr_array_index(list, 0))->id, ==, "f1");
  g_ptr_array_unref(list);
  rh_filter_free(f);

  /* 关键字：ASCII 与中文各一条路径 */
  f = rh_filter_new();
  f->kw = "Mario";
  list = NULL;
  assert_ok(store_game_list(ctx, f, &list), "filter kw ascii");
  g_assert_cmpuint(list_len(list), ==, 2);
  g_ptr_array_unref(list);
  rh_filter_free(f);

  f = rh_filter_new();
  f->kw = "马里奥";
  list = NULL;
  assert_ok(store_game_list(ctx, f, &list), "filter kw chinese");
  g_assert_cmpuint(list_len(list), ==, 1);
  g_ptr_array_unref(list);
  rh_filter_free(f);

  /* 排序：year desc */
  f = rh_filter_new();
  f->sort = "year";
  f->desc = TRUE;
  list = NULL;
  assert_ok(store_game_list(ctx, f, &list), "sort year desc");
  g_assert_cmpuint(list_len(list), ==, 4);
  g_assert_cmpint(((RhGame *) g_ptr_array_index(list, 0))->year, ==, 1990);
  g_ptr_array_unref(list);
  rh_filter_free(f);

  /* 非法排序键要报错，不能静默按 name 排 */
  f = rh_filter_new();
  f->sort = "bogus";
  list = NULL;
  e = store_game_list(ctx, f, &list);
  g_assert_false(rh_err_ok(&e));
  g_assert_cmpint(e.code, ==, RH_ERR_CONFIG);
  rh_filter_free(f);

  assert_ok(store_game_count(ctx, &n), "count");
  g_assert_cmpint(n, ==, 4);

  close_ctx();
}

/* ---------------------------------------------------------------- DAT */

static void test_dat(void)
{
  RhDatEntry *e1 = rh_dat_entry_new();
  RhDatEntry *e2 = rh_dat_entry_new();
  GPtrArray *entries = g_ptr_array_new_with_free_func(
    (GDestroyNotify) rh_dat_entry_free);
  GPtrArray *hits;
  gint64 dat_id = 0, count = 0;
  RhErr e;

  open_ctx("dat");

  e1->game_name = g_strdup("Super Mario Bros");
  e1->rom_name = g_strdup("Super Mario Bros.nes");
  e1->size = 40960;
  e1->crc32 = g_strdup("ABCD1234");

  e2->game_name = g_strdup("Zelda");
  e2->rom_name = g_strdup("Zelda.nes");
  e2->size = 81920;
  e2->sha1 = g_strdup("00112233445566778899AABBCCDDEEFF00112233");

  g_ptr_array_add(entries, e1);
  g_ptr_array_add(entries, e2);

  assert_ok(store_dat_begin(ctx, "nes", "/dat/Nintendo - NES.dat",
                            "Nintendo - NES", "20240101", &dat_id),
            "dat begin");
  g_assert_cmpint(dat_id, >, 0);

  assert_ok(store_dat_entries_add(ctx, dat_id, entries), "dat entries");
  assert_ok(store_dat_finish(ctx, dat_id, 2), "dat finish");

  assert_ok(store_dat_count(ctx, &count), "dat count");
  g_assert_cmpint(count, ==, 2);

  hits = NULL;
  assert_ok(store_dat_find_by_hash(ctx, "crc32", "abcd1234", &hits),
            "dat find crc32");                      /* 大小写不敏感 */
  g_assert_cmpuint(list_len(hits), ==, 1);
  g_assert_cmpstr(((RhDatMatch *) g_ptr_array_index(hits, 0))->game_name,
                  ==, "Super Mario Bros");
  g_ptr_array_unref(hits);

  hits = NULL;
  assert_ok(store_dat_find_by_hash(ctx, "sha1",
            "00112233445566778899aabbccddeeff00112233", &hits), "dat find sha1");
  g_assert_cmpuint(list_len(hits), ==, 1);
  g_ptr_array_unref(hits);

  hits = NULL;
  assert_ok(store_dat_find_by_hash(ctx, "md5", "0000", &hits), "dat find miss");
  g_assert_cmpuint(list_len(hits), ==, 0);
  g_ptr_array_unref(hits);

  hits = NULL;
  e = store_dat_find_by_hash(ctx, "crc64", "0000", &hits);
  g_assert_false(rh_err_ok(&e));
  g_assert_cmpint(e.code, ==, RH_ERR_UNSUPPORTED);

  g_ptr_array_unref(entries);
  close_ctx();
}

/* ---------------------------------------------------------------- 作业 */

static void test_jobs(void)
{
  gint64 job_id = 0;
  GPtrArray *jobs, *items;
  RhJob *job = NULL;

  open_ctx("jobs");

  assert_ok(store_job_create(ctx, "import", 5, &job_id), "job create");
  g_assert_cmpint(job_id, >, 0);

  assert_ok(store_job_update(ctx, job_id, "running", 2, NULL), "job running");

  jobs = NULL;
  assert_ok(store_job_list_unfinished(ctx, &jobs), "unfinished jobs");
  g_assert_cmpuint(list_len(jobs), ==, 1);
  g_assert_cmpstr(((RhJob *) g_ptr_array_index(jobs, 0))->state, ==, "running");
  g_ptr_array_unref(jobs);

  for (int i = 0; i < 2; i++) {
    RhJobItem *it = rh_job_item_new();
    it->job_id = job_id;
    it->seq = i;
    it->src = g_strdup_printf("/src/file%d.bin", i);
    it->action = g_strdup("rom");
    assert_ok(store_job_item_add(ctx, it), "job item add");
    rh_job_item_free(it);
  }

  assert_ok(store_job_item_update(ctx, job_id, 0, "done", "ok"),
            "job item update");

  items = NULL;
  assert_ok(store_job_items_list(ctx, job_id, &items), "job items list");
  g_assert_cmpuint(list_len(items), ==, 2);
  g_assert_cmpstr(((RhJobItem *) g_ptr_array_index(items, 0))->state, ==, "done");
  g_ptr_array_unref(items);

  assert_ok(store_job_update(ctx, job_id, "done", 5, NULL), "job done");

  assert_ok(store_job_get(ctx, job_id, &job), "job get");
  g_assert_cmpstr(job->state, ==, "done");
  g_assert_cmpint(job->done, ==, 5);
  g_assert_nonnull(job->finished_at);
  rh_job_free(job);

  jobs = NULL;
  assert_ok(store_job_list_unfinished(ctx, &jobs), "unfinished after done");
  g_assert_cmpuint(list_len(jobs), ==, 0);
  g_ptr_array_unref(jobs);

  close_ctx();
}

/* ---------------------------------------------------------------- 检索 */

static void test_search_fts(void)
{
  GPtrArray *list;
  RhGame *g;
  RhErr e;

  open_ctx("search");

  g = mk_game("s1", "nes", "Super Mario Bros", "nes/s1");
  assert_ok(store_game_upsert(ctx, g), "s1");
  rh_game_free(g);

  list = NULL;
  e = store_search_fts(ctx, "Zelda", &list);
  assert_ok(e, "search miss");
  g_assert_cmpuint(list_len(list), ==, 0);
  g_ptr_array_unref(list);

  list = NULL;
  assert_ok(store_search_fts(ctx, "Mario", &list), "search hit");
  g_assert_cmpuint(list_len(list), ==, 1);
  g_ptr_array_unref(list);

  /* 引号必须被转义：不能因为用户输入带 " 就抛 FTS 语法错 */
  list = NULL;
  assert_ok(store_search_fts(ctx, "Mario\"quote", &list), "search with quote");
  g_ptr_array_unref(list);

  close_ctx();
}

/* ---------------------------------------------------------------- main */

static void cleanup(void)
{
  GDir *d = g_dir_open(tmpdir, 0, NULL);

  if (d) {
    const gchar *name;
    while ((name = g_dir_read_name(d)) != NULL) {
      gchar *sub = g_build_filename(tmpdir, name, NULL);
      GDir *sd = g_dir_open(sub, 0, NULL);
      if (sd) {
        const gchar *sn;
        while ((sn = g_dir_read_name(sd)) != NULL) {
          gchar *f = g_build_filename(sub, sn, NULL);
          g_remove(f);
          g_free(f);
        }
        g_dir_close(sd);
        g_rmdir(sub);
      }
      g_free(sub);
    }
    g_dir_close(d);
    g_rmdir(tmpdir);
  }
  g_free(tmpdir);
}

int main(int argc, char **argv)
{
  int rc;

  g_test_init(&argc, &argv, NULL);

  tmpdir = g_dir_make_tmp("rh-crud-XXXXXX", NULL);
  g_assert_nonnull(tmpdir);

  g_test_add_func("/store/crud", test_crud_roundtrip);
  g_test_add_func("/store/auto-id", test_auto_id);
  g_test_add_func("/store/dir-path-unique", test_dir_path_unique);
  g_test_add_func("/store/children-and-cascade", test_children_and_cascade);
  g_test_add_func("/store/filters", test_filters);
  g_test_add_func("/store/dat", test_dat);
  g_test_add_func("/store/jobs", test_jobs);
  g_test_add_func("/store/search", test_search_fts);

  rc = g_test_run();
  cleanup();
  return rc;
}
