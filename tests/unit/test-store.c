/*
 * test-store.c — store_open / schema v1 单测（M0 验收）
 *
 * 只走 store_* API：本项目约定"除 store.c 外不出现 SQL"，测试也不例外。
 */
#include <glib.h>
#include <glib/gstdio.h>

#include "rh-ctx.h"
#include "rh-err.h"
#include "store.h"
#include "store-schema.h"

#define RH_STORE_SCHEMA_VERSION_EXPECTED 1

static gchar *tmpdir;
static gchar *db_path;

static void test_open_creates_schema(void)
{
  RhCtx *ctx = rh_ctx_new();
  RhErr  e;
  gint64 n = -1;

  e = store_open(ctx, db_path, STORE_OPEN_CREATE);
  g_assert_true(rh_err_ok(&e));
  g_assert_nonnull(ctx->db);

  e = store_game_count(ctx, &n);
  g_assert_true(rh_err_ok(&e));
  g_assert_cmpint(n, ==, 0);

  store_close(ctx);
  g_assert_null(ctx->db);
  rh_ctx_free(ctx);
}

/* 再打开一次：user_version 已是 1，迁移应直接返回 OK，不能重建表 */
static void test_reopen_is_idempotent(void)
{
  RhCtx *ctx = rh_ctx_new();
  RhErr  e;
  gint64 n = -1;

  e = store_open(ctx, db_path, 0);
  g_assert_true(rh_err_ok(&e));

  e = store_migrate(ctx);
  g_assert_true(rh_err_ok(&e));

  e = store_game_count(ctx, &n);
  g_assert_true(rh_err_ok(&e));
  g_assert_cmpint(n, ==, 0);

  store_close(ctx);
  rh_ctx_free(ctx);
}

/* 只读打开一个不存在的库必须失败，而不是悄悄建一个 */
static void test_readonly_missing_db(void)
{
  RhCtx *ctx = rh_ctx_new();
  gchar *missing = g_build_filename(tmpdir, "missing.db", NULL);
  RhErr  e = store_open(ctx, missing, STORE_OPEN_READONLY);

  g_assert_false(rh_err_ok(&e));
  g_assert_null(ctx->db);

  g_free(missing);
  rh_ctx_free(ctx);
}

static void test_tx_commit_and_rollback(void)
{
  RhCtx *ctx = rh_ctx_new();
  RhErr  e;

  e = store_open(ctx, db_path, 0);
  g_assert_true(rh_err_ok(&e));

  e = store_tx_begin(ctx);
  g_assert_true(rh_err_ok(&e));
  g_assert_true(ctx->in_tx);

  e = store_tx_rollback(ctx);
  g_assert_true(rh_err_ok(&e));
  g_assert_false(ctx->in_tx);

  /* 嵌套事务必须被拒绝，而不是静默开始第二个 */
  e = store_tx_begin(ctx);
  g_assert_true(rh_err_ok(&e));
  e = store_tx_begin(ctx);
  g_assert_false(rh_err_ok(&e));
  g_assert_cmpint(e.code, ==, RH_ERR_CONFLICT);
  e = store_tx_commit(ctx);
  g_assert_true(rh_err_ok(&e));
  g_assert_false(ctx->in_tx);

  store_close(ctx);
  rh_ctx_free(ctx);
}

static void cleanup(void)
{
  if (db_path) {
    gchar *wal = g_strconcat(db_path, "-wal", NULL);
    gchar *shm = g_strconcat(db_path, "-shm", NULL);
    g_remove(wal);
    g_remove(shm);
    g_free(wal);
    g_free(shm);
    g_remove(db_path);
    g_free(db_path);
  }
  if (tmpdir) {
    g_rmdir(tmpdir);
    g_free(tmpdir);
  }
}

int main(int argc, char **argv)
{
  int rc;

  g_test_init(&argc, &argv, NULL);

  tmpdir = g_dir_make_tmp("rh-store-XXXXXX", NULL);
  g_assert_nonnull(tmpdir);
  db_path = g_build_filename(tmpdir, "romhamster.db", NULL);

  g_assert_cmpint(RH_STORE_SCHEMA_VERSION,
                  ==,
                  RH_STORE_SCHEMA_VERSION_EXPECTED);

  g_test_add_func("/store/create-schema", test_open_creates_schema);
  g_test_add_func("/store/reopen", test_reopen_is_idempotent);
  g_test_add_func("/store/readonly-missing", test_readonly_missing_db);
  g_test_add_func("/store/transactions", test_tx_commit_and_rollback);

  rc = g_test_run();
  cleanup();
  return rc;
}
