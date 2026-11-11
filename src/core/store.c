#include "store.h"

#include <sqlite3.h>

#include "store-schema.h"

/* ================================================================= SQL 归类
 *
 * 本项目所有 SQL 只允许出现在这个文件里。其他模块要数据必须走 store_* API。
 * ========================================================================= */

/* ---------------------------------------------------------- schema v1 建表 */

static const char *const v1_tables[] = {
  "CREATE TABLE games ("
  "  id             TEXT PRIMARY KEY,"
  "  platform       TEXT NOT NULL,"
  "  name           TEXT NOT NULL,"
  "  game_main_name TEXT,"
  "  dir_path       TEXT NOT NULL UNIQUE,"
  "  developer      TEXT,"
  "  publisher      TEXT,"
  "  year           INTEGER,"
  "  genre          TEXT,"
  "  players        INTEGER,"
  "  rating         REAL,"
  "  region         TEXT,"
  "  description    TEXT,"
  "  is_multi_disc  INTEGER NOT NULL DEFAULT 0 CHECK (is_multi_disc IN (0,1)),"
  "  is_arcade      INTEGER NOT NULL DEFAULT 0 CHECK (is_arcade IN (0,1)),"
  "  rom_format     TEXT NOT NULL DEFAULT 'raw',"
  "  rom_status     TEXT NOT NULL DEFAULT 'unknown'"
  "                 CHECK (rom_status IN ('good','baddump','unknown')),"
  "  dat_source     TEXT,"
  "  patch_type     TEXT NOT NULL DEFAULT ''"
  "                 CHECK (patch_type IN ('','generic','platform','mixed')),"
  "  created_at     TEXT NOT NULL,"
  "  updated_at     TEXT NOT NULL"
  ")",

  "CREATE TABLE roms ("
  "  game_id  TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,"
  "  filename TEXT NOT NULL,"
  "  disc_no  INTEGER,"
  "  size     INTEGER,"
  "  crc32    TEXT,"
  "  md5      TEXT,"
  "  sha1     TEXT,"
  "  sort     INTEGER NOT NULL DEFAULT 0,"
  "  PRIMARY KEY (game_id, filename)"
  ")",

  "CREATE TABLE media ("
  "  game_id  TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,"
  "  kind     TEXT NOT NULL CHECK (kind IN"
  "           ('covers','screenshots','videos','marquees','miximages','intro','patcher')),"
  "  filename TEXT NOT NULL,"
  "  size     INTEGER,"
  "  sort     INTEGER NOT NULL DEFAULT 0,"
  "  PRIMARY KEY (game_id, kind, filename)"
  ")",

  "CREATE TABLE tags (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE)",

  "CREATE TABLE game_tags ("
  "  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,"
  "  tag_id  INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,"
  "  PRIMARY KEY (game_id, tag_id)"
  ")",

  "CREATE TABLE dats ("
  "  id           INTEGER PRIMARY KEY,"
  "  platform     TEXT NOT NULL,"
  "  path         TEXT NOT NULL,"
  "  name         TEXT,"
  "  version      TEXT,"
  "  imported_at  TEXT NOT NULL,"
  "  entry_count  INTEGER,"
  "  UNIQUE (platform, path)"
  ")",

  "CREATE TABLE dat_entries ("
  "  dat_id    INTEGER NOT NULL REFERENCES dats(id) ON DELETE CASCADE,"
  "  game_name TEXT,"
  "  rom_name  TEXT NOT NULL,"
  "  size      INTEGER,"
  "  crc32     TEXT,"
  "  md5       TEXT,"
  "  sha1      TEXT,"
  "  PRIMARY KEY (dat_id, rom_name)"
  ")",

  "CREATE TABLE jobs ("
  "  id          INTEGER PRIMARY KEY,"
  "  kind        TEXT NOT NULL,"
  "  state       TEXT NOT NULL,"
  "  total       INTEGER NOT NULL DEFAULT 0,"
  "  done        INTEGER NOT NULL DEFAULT 0,"
  "  created_at  TEXT NOT NULL,"
  "  finished_at TEXT,"
  "  error       TEXT"
  ")",

  "CREATE TABLE job_items ("
  "  job_id     INTEGER NOT NULL REFERENCES jobs(id) ON DELETE CASCADE,"
  "  seq        INTEGER NOT NULL,"
  "  src        TEXT NOT NULL,"
  "  action     TEXT,"
  "  platform   TEXT,"
  "  target_id  TEXT,"
  "  state      TEXT NOT NULL DEFAULT 'pending',"
  "  message    TEXT,"
  "  PRIMARY KEY (job_id, seq)"
  ")",

  "CREATE INDEX idx_games_platform ON games(platform)",
  "CREATE INDEX idx_games_name     ON games(name)",
  "CREATE INDEX idx_games_year     ON games(year)",
  "CREATE INDEX idx_games_rating   ON games(rating)",
  "CREATE INDEX idx_games_status   ON games(rom_status)",
  "CREATE INDEX idx_roms_disc      ON roms(game_id, disc_no)",
  "CREATE INDEX idx_dat_crc32      ON dat_entries(crc32)",
  "CREATE INDEX idx_dat_md5        ON dat_entries(md5)",
  "CREATE INDEX idx_dat_sha1       ON dat_entries(sha1)",
};

/* FTS5 用 external content 表，需要三个触发器同步 */
static const char *const v1_fts[] = {
  "CREATE TRIGGER games_fts_ai AFTER INSERT ON games BEGIN"
  "  INSERT INTO games_fts(rowid, name, game_main_name, description)"
  "  VALUES (new.rowid, new.name, new.game_main_name, new.description);"
  "END",

  "CREATE TRIGGER games_fts_ad AFTER DELETE ON games BEGIN"
  "  INSERT INTO games_fts(games_fts, rowid, name, game_main_name, description)"
  "  VALUES ('delete', old.rowid, old.name, old.game_main_name, old.description);"
  "END",

  "CREATE TRIGGER games_fts_au AFTER UPDATE ON games BEGIN"
  "  INSERT INTO games_fts(games_fts, rowid, name, game_main_name, description)"
  "  VALUES ('delete', old.rowid, old.name, old.game_main_name, old.description);"
  "  INSERT INTO games_fts(rowid, name, game_main_name, description)"
  "  VALUES (new.rowid, new.name, new.game_main_name, new.description);"
  "END",
};

/* ---------------------------------------------------------------- 错误映射 */

static RhErr err_from_db(sqlite3 *db, const char *what)
{
  int   ext = sqlite3_extended_errcode(db);
  const char *msg = sqlite3_errmsg(db);

  switch (ext) {
  case SQLITE_BUSY:
  case SQLITE_LOCKED:
    return rh_fail(RH_ERR_DB_BUSY,
                   "%s: another instance is writing (%s)", what, msg);
  case SQLITE_CANTOPEN:
  case SQLITE_FULL:
  case SQLITE_READONLY:
    return rh_fail(RH_ERR_IO, "%s: %s", what, msg);
  case SQLITE_NOTADB:
  case SQLITE_CORRUPT:
    return rh_fail(RH_ERR_SCHEMA, "%s: %s", what, msg);
  default:
    return rh_fail(RH_ERR_DB, "%s: %s (code %d)", what, msg, ext);
  }
}

static RhErr exec_stmt(sqlite3 *db, const char *sql, const char *what)
{
  char *errmsg = NULL;

  if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
    RhErr e = err_from_db(db, what);
    if (errmsg) {
      /* sqlite3 的 exec 错误消息比 errmsg() 更具体，补进去 */
      g_strlcpy(e.msg, errmsg, sizeof e.msg);
      sqlite3_free(errmsg);
    }
    return e;
  }
  return rh_ok();
}

/* --------------------------------------------------------------- FTS 能力探测 */

static gboolean probe_trigram(sqlite3 *db)
{
  const char *create =
    "CREATE VIRTUAL TABLE temp.rh_fts_probe"
    " USING fts5(x, tokenize=\"trigram\")";

  if (sqlite3_exec(db, create, NULL, NULL, NULL) != SQLITE_OK) {
    sqlite3_exec(db, "DROP TABLE IF EXISTS temp.rh_fts_probe", NULL, NULL, NULL);
    return FALSE;
  }
  sqlite3_exec(db, "DROP TABLE temp.rh_fts_probe", NULL, NULL, NULL);
  return TRUE;
}

/* ---------------------------------------------------------------- 迁移 */

static RhErr create_v1(RhCtx *ctx, sqlite3 *db)
{
  RhErr e;
  gboolean trigram = probe_trigram(db);
  gchar *fts_sql;

  for (guint i = 0; i < G_N_ELEMENTS(v1_tables); i++) {
    e = exec_stmt(db, v1_tables[i], "create schema v1");
    if (!rh_err_ok(&e))
      return e;
  }

  /* trigram（SQLite >= 3.34）对中文子串友好；探测不到就降级 unicode61，
   * 检索层再按 ctx->fts_trigram 决定是否退化为 LIKE（ARCHITECTURE §3.3） */
  if (trigram)
    fts_sql = g_strdup(
      "CREATE VIRTUAL TABLE games_fts USING fts5("
      "  name, game_main_name, description,"
      "  content='games', content_rowid='rowid', tokenize=\"trigram\")");
  else
    fts_sql = g_strdup(
      "CREATE VIRTUAL TABLE games_fts USING fts5("
      "  name, game_main_name, description,"
      "  content='games', content_rowid='rowid')");

  e = exec_stmt(db, fts_sql, "create games_fts");
  g_free(fts_sql);
  if (!rh_err_ok(&e))
    return e;

  for (guint i = 0; i < G_N_ELEMENTS(v1_fts); i++) {
    e = exec_stmt(db, v1_fts[i], "create fts triggers");
    if (!rh_err_ok(&e))
      return e;
  }

  ctx->fts_trigram = trigram;
  return exec_stmt(db, "PRAGMA user_version = 1", "set user_version");
}

RhErr store_migrate(RhCtx *ctx)
{
  sqlite3 *db = ctx->db;
  sqlite3_stmt *st = NULL;
  int version = 0;
  RhErr e;

  if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, NULL) != SQLITE_OK)
    return err_from_db(db, "read user_version");
  if (sqlite3_step(st) == SQLITE_ROW)
    version = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);

  if (version == RH_STORE_SCHEMA_VERSION)
    return rh_ok();
  if (version > RH_STORE_SCHEMA_VERSION)
    return rh_fail(RH_ERR_SCHEMA,
                   "database schema v%d is newer than this build (v%d)",
                   version, RH_STORE_SCHEMA_VERSION);

  /* 单事务内建 v1：要么全建成，要么一个表都没有 */
  e = store_tx_begin(ctx);
  if (!rh_err_ok(&e))
    return e;

  e = create_v1(ctx, db);
  if (!rh_err_ok(&e)) {
    store_tx_rollback(ctx);
    return e;
  }
  return store_tx_commit(ctx);
}

/* ---------------------------------------------------------------- 生命周期 */

RhErr store_open(RhCtx *ctx, const char *db_path, int flags)
{
  sqlite3 *db = NULL;
  gchar   *dir = NULL;
  int      oflags;
  RhErr    e;

  if (!ctx || !db_path || !*db_path)
    return rh_fail(RH_ERR_CONFIG, "store_open: empty database path");
  if (ctx->db)
    return rh_fail(RH_ERR_CONFLICT, "store_open: database already open");

  if (!(flags & STORE_OPEN_READONLY)) {
    dir = g_path_get_dirname(db_path);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
      e = rh_fail(RH_ERR_IO, "cannot create directory %s", dir);
      g_free(dir);
      return e;
    }
    g_free(dir);
  }

  oflags = (flags & STORE_OPEN_READONLY)
         ? SQLITE_OPEN_READONLY
         : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);

  if (sqlite3_open_v2(db_path, &db, oflags, NULL) != SQLITE_OK) {
    e = rh_fail(RH_ERR_IO, "cannot open %s: %s", db_path, sqlite3_errmsg(db));
    sqlite3_close(db);
    return e;
  }

  /* 没有锁文件，靠 SQLite 自身的写锁串行化（ARCHITECTURE §13） */
  sqlite3_busy_timeout(db, 5000);

  if (!(flags & STORE_OPEN_READONLY)) {
    static const char *const pragmas[] = {
      "PRAGMA journal_mode = WAL",
      "PRAGMA foreign_keys = ON",
      "PRAGMA synchronous = NORMAL",
    };
    for (guint i = 0; i < G_N_ELEMENTS(pragmas); i++) {
      e = exec_stmt(db, pragmas[i], "apply pragma");
      if (!rh_err_ok(&e)) {
        sqlite3_close(db);
        return e;
      }
    }
  }

  ctx->db = db;
  ctx->db_path = g_strdup(db_path);

  e = store_migrate(ctx);
  if (!rh_err_ok(&e)) {
    store_close(ctx);
    return e;
  }
  return rh_ok();
}

void store_close(RhCtx *ctx)
{
  if (!ctx)
    return;
  if (ctx->db) {
    if (ctx->in_tx) {
      sqlite3_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
      ctx->in_tx = FALSE;
    }
    sqlite3_close(ctx->db);
    ctx->db = NULL;
  }
  g_clear_pointer(&ctx->db_path, g_free);
}

/* ---------------------------------------------------------------- 事务 */

RhErr store_tx_begin(RhCtx *ctx)
{
  RhErr e;

  if (!ctx || !ctx->db)
    return rh_fail(RH_ERR_DB, "transaction without an open database");
  if (ctx->in_tx)
    return rh_fail(RH_ERR_CONFLICT, "nested transaction is not supported");

  e = exec_stmt(ctx->db, "BEGIN IMMEDIATE", "begin transaction");
  if (rh_err_ok(&e))
    ctx->in_tx = TRUE;
  return e;
}

RhErr store_tx_commit(RhCtx *ctx)
{
  RhErr e;

  if (!ctx || !ctx->db || !ctx->in_tx)
    return rh_fail(RH_ERR_DB, "commit without an open transaction");

  e = exec_stmt(ctx->db, "COMMIT", "commit transaction");
  ctx->in_tx = FALSE;    /* 失败也已结束事务（sqlite 会回滚剩余语句） */
  return e;
}

RhErr store_tx_rollback(RhCtx *ctx)
{
  RhErr e;

  if (!ctx || !ctx->db)
    return rh_fail(RH_ERR_DB, "rollback without an open database");
  if (!ctx->in_tx)
    return rh_ok();

  e = exec_stmt(ctx->db, "ROLLBACK", "rollback transaction");
  ctx->in_tx = FALSE;
  return e;
}

/* ---------------------------------------------------------------- 查询 */

RhErr store_game_count(RhCtx *ctx, gint64 *out)
{
  sqlite3_stmt *st = NULL;

  if (!ctx || !ctx->db || !out)
    return rh_fail(RH_ERR_CONFIG, "store_game_count: bad arguments");

  if (sqlite3_prepare_v2(ctx->db, "SELECT count(*) FROM games", -1, &st, NULL)
      != SQLITE_OK)
    return err_from_db(ctx->db, "count games");

  *out = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : 0;
  sqlite3_finalize(st);
  return rh_ok();
}
