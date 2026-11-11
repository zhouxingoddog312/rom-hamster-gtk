#include "store.h"

#include <sqlite3.h>
#include <string.h>

#include "store-schema.h"

/* ================================================================= SQL 归类
 *
 * 本项目所有 SQL 只允许出现在这个文件里。其他模块要数据必须走 store_* API
 * （ARCHITECTURE §5.1 硬规则）。
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
  int   primary = ext & 0xff;
  const char *msg = sqlite3_errmsg(db);

  switch (primary) {
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
  case SQLITE_CONSTRAINT:
    if (ext == SQLITE_CONSTRAINT_FOREIGNKEY)
      return rh_fail(RH_ERR_CONFLICT, "%s: %s", what, msg);
    return rh_fail(RH_ERR_EXISTS, "%s: %s", what, msg);
  default:
    return rh_fail(RH_ERR_DB, "%s: %s (code %d)", what, msg, ext);
  }
}

static RhErr err_no_db(void)
{
  return rh_fail(RH_ERR_DB, "no open database");
}

static RhErr prepare(sqlite3 *db, const char *sql, sqlite3_stmt **st,
                     const char *what)
{
  if (sqlite3_prepare_v2(db, sql, -1, st, NULL) != SQLITE_OK)
    return err_from_db(db, what);
  return rh_ok();
}

/* 执行一条只要 DONE 的语句 */
static RhErr step_done(sqlite3 *db, sqlite3_stmt *st, const char *what)
{
  RhErr e = rh_ok();

  if (sqlite3_step(st) != SQLITE_DONE)
    e = err_from_db(db, what);
  sqlite3_finalize(st);
  return e;
}

static RhErr exec_stmt(sqlite3 *db, const char *sql, const char *what)
{
  char *errmsg = NULL;

  if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
    RhErr e = err_from_db(db, what);
    if (errmsg) {
      g_strlcpy(e.msg, errmsg, sizeof e.msg);
      sqlite3_free(errmsg);
    }
    return e;
  }
  return rh_ok();
}

/* ---------------------------------------------------------------- 绑定助手 */

static void bind_str(sqlite3_stmt *st, int idx, const char *s)
{
  if (s && *s)
    sqlite3_bind_text(st, idx, s, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null(st, idx);
}

/* 0 视为"未知" → NULL */
static void bind_int_or_null(sqlite3_stmt *st, int idx, int v)
{
  if (v <= 0)
    sqlite3_bind_null(st, idx);
  else
    sqlite3_bind_int(st, idx, v);
}

/* <0 视为"未知" → NULL */
static void bind_rating(sqlite3_stmt *st, int idx, double v)
{
  if (v < 0)
    sqlite3_bind_null(st, idx);
  else
    sqlite3_bind_double(st, idx, v);
}

static void bind_size(sqlite3_stmt *st, int idx, gint64 v)
{
  if (v < 0)
    sqlite3_bind_null(st, idx);
  else
    sqlite3_bind_int64(st, idx, v);
}

/* ---------------------------------------------------------------- 列定义 */

#define GAME_COLUMNS \
  "id, platform, name, game_main_name, dir_path, developer, publisher, year, " \
  "genre, players, rating, region, description, is_multi_disc, is_arcade, " \
  "rom_format, rom_status, dat_source, patch_type, created_at, updated_at"

#define GAME_SELECT "SELECT " GAME_COLUMNS " FROM games"

static const char *const game_upsert_sql =
  "INSERT INTO games (" GAME_COLUMNS ") VALUES ("
  "?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?"
  ") ON CONFLICT(id) DO UPDATE SET"
  "  platform       = excluded.platform,"
  "  name           = excluded.name,"
  "  game_main_name = excluded.game_main_name,"
  "  dir_path       = excluded.dir_path,"
  "  developer      = excluded.developer,"
  "  publisher      = excluded.publisher,"
  "  year           = excluded.year,"
  "  genre          = excluded.genre,"
  "  players        = excluded.players,"
  "  rating         = excluded.rating,"
  "  region         = excluded.region,"
  "  description    = excluded.description,"
  "  is_multi_disc  = excluded.is_multi_disc,"
  "  is_arcade      = excluded.is_arcade,"
  "  rom_format     = excluded.rom_format,"
  "  rom_status     = excluded.rom_status,"
  "  dat_source     = excluded.dat_source,"
  "  patch_type     = excluded.patch_type,"
  "  updated_at     = excluded.updated_at";

static gchar *col_str(sqlite3_stmt *st, int i)
{
  return sqlite3_column_text(st, i) ? g_strdup((const char *)
                                               sqlite3_column_text(st, i))
                                    : NULL;
}

static RhGame *row_to_game(sqlite3_stmt *st)
{
  RhGame *g = rh_game_new();
  double  rating = sqlite3_column_double(st, 10);

  g->id             = col_str(st, 0);
  g->platform       = col_str(st, 1);
  g->name           = col_str(st, 2);
  g->game_main_name = col_str(st, 3);
  g->dir_path       = col_str(st, 4);
  g->developer      = col_str(st, 5);
  g->publisher      = col_str(st, 6);
  g->year           = sqlite3_column_int(st, 7);
  g->genre          = col_str(st, 8);
  g->players        = sqlite3_column_int(st, 9);
  g->rating         = (sqlite3_column_type(st, 10) == SQLITE_NULL) ? -1.0 : rating;
  g->region         = col_str(st, 11);
  g->description    = col_str(st, 12);
  g->is_multi_disc  = sqlite3_column_int(st, 13) != 0;
  g->is_arcade      = sqlite3_column_int(st, 14) != 0;

  g_free(g->rom_format);
  g_free(g->rom_status);
  g_free(g->patch_type);
  g->rom_format = col_str(st, 15);
  g->rom_status = col_str(st, 16);
  g->dat_source = col_str(st, 17);
  g->patch_type = col_str(st, 18);
  g->created_at = col_str(st, 19);
  g->updated_at = col_str(st, 20);
  return g;
}

/* -------------------------------------------------- 语句构造期的值绑定队列 */

typedef struct {
  gboolean    text;
  const char *s;       /* 由本结构拥有 */
  gint64      n;
} BindVal;

static void bindval_free(gpointer p)
{
  BindVal *v = p;

  if (!v)
    return;
  g_free((gchar *) v->s);
  g_free(v);
}

/* 复制一份：调用方接着 g_free 自己的临时串也不会影响绑定 */
static void push_text(GPtrArray *b, const char *s)
{
  BindVal *v = g_new0(BindVal, 1);

  v->text = TRUE;
  v->s = g_strdup(s);
  g_ptr_array_add(b, v);
}

static void push_int(GPtrArray *b, gint64 n)
{
  BindVal *v = g_new0(BindVal, 1);
  v->text = FALSE;
  v->n = n;
  g_ptr_array_add(b, v);
}

static RhErr apply_binds(sqlite3_stmt *st, GPtrArray *binds, const char *what)
{
  int n = sqlite3_bind_parameter_count(st);

  if (n != (int) binds->len)
    return rh_fail(RH_ERR_INTERNAL,
                   "%s: %d placeholders but %u values bound", what, n,
                   binds->len);

  for (guint i = 0; i < binds->len; i++) {
    BindVal *v = g_ptr_array_index(binds, i);
    if (v->text)
      bind_str(st, (int) i + 1, v->s);
    else
      sqlite3_bind_int64(st, (int) i + 1, v->n);
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
   * 检索层再按 ctx->fts_trigram 退化为 LIKE（ARCHITECTURE §3.3） */
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
  sqlite3 *db;
  sqlite3_stmt *st = NULL;
  int version = 0;
  RhErr e;

  if (!ctx || !ctx->db)
    return err_no_db();
  db = ctx->db;

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
    return err_no_db();
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
    return err_no_db();
  if (!ctx->in_tx)
    return rh_ok();

  e = exec_stmt(ctx->db, "ROLLBACK", "rollback transaction");
  ctx->in_tx = FALSE;
  return e;
}

/* 供"要么全成要么全不成"的复合操作：调用方可能已在自己的事务里 */
static RhErr tx_begin_if_needed(RhCtx *ctx, gboolean *owned)
{
  *owned = !ctx->in_tx;
  return *owned ? store_tx_begin(ctx) : rh_ok();
}

/* ================================================================= 游戏记录 */

RhErr store_game_get(RhCtx *ctx, const char *id, RhGame **out)
{
  sqlite3_stmt *st = NULL;
  RhErr e;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!id || !*id || !out)
    return rh_fail(RH_ERR_CONFIG, "store_game_get: bad arguments");
  *out = NULL;

  e = prepare(ctx->db, GAME_SELECT " WHERE id = ?", &st, "get game");
  if (!rh_err_ok(&e))
    return e;

  bind_str(st, 1, id);
  if (sqlite3_step(st) == SQLITE_ROW) {
    *out = row_to_game(st);
    e = rh_ok();
  } else {
    e = rh_fail(RH_ERR_NOT_FOUND, "game %s not found", id);
  }
  sqlite3_finalize(st);
  return e;
}

RhErr store_game_upsert(RhCtx *ctx, const RhGame *g)
{
  sqlite3_stmt *st = NULL;
  RhErr e;
  gchar *id = NULL;
  gchar *now = NULL;
  const gchar *created_at, *updated_at;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!g || !g->platform || !*g->platform || !g->name || !*g->name ||
      !g->dir_path || !*g->dir_path)
    return rh_fail(RH_ERR_CONFIG, "game needs platform, name and dir_path");

  if (g->id && *g->id)
    id = g_strdup(g->id);
  else {
    id = rh_game_gen_id(g->platform, g->dir_path);
    if (!id)
      return rh_fail(RH_ERR_INTERNAL, "cannot derive game id");
  }

  now = rh_now_iso();
  created_at = g->created_at && *g->created_at ? g->created_at : now;
  updated_at = g->updated_at && *g->updated_at ? g->updated_at : now;

  e = prepare(ctx->db, game_upsert_sql, &st, "upsert game");
  if (!rh_err_ok(&e)) {
    g_free(id);
    g_free(now);
    return e;
  }

  bind_str(st, 1, id);
  bind_str(st, 2, g->platform);
  bind_str(st, 3, g->name);
  bind_str(st, 4, g->game_main_name);
  bind_str(st, 5, g->dir_path);
  bind_str(st, 6, g->developer);
  bind_str(st, 7, g->publisher);
  bind_int_or_null(st, 8, g->year);
  bind_str(st, 9, g->genre);
  bind_int_or_null(st, 10, g->players);
  bind_rating(st, 11, g->rating);
  bind_str(st, 12, g->region);
  bind_str(st, 13, g->description);
  sqlite3_bind_int(st, 14, g->is_multi_disc ? 1 : 0);
  sqlite3_bind_int(st, 15, g->is_arcade ? 1 : 0);
  bind_str(st, 16, g->rom_format && *g->rom_format ? g->rom_format : "raw");
  bind_str(st, 17, g->rom_status && *g->rom_status ? g->rom_status : "unknown");
  bind_str(st, 18, g->dat_source);
  /* patch_type 是 NOT NULL：空字符串是合法值（"无补丁"），不能绑成 NULL */
  sqlite3_bind_text(st, 19, g->patch_type ? g->patch_type : "", -1,
                    SQLITE_STATIC);
  bind_str(st, 20, created_at);
  bind_str(st, 21, updated_at);

  e = step_done(ctx->db, st, "upsert game");
  g_free(id);
  g_free(now);
  return e;
}

RhErr store_game_delete(RhCtx *ctx, const char *id)
{
  sqlite3_stmt *st = NULL;
  RhErr e;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!id || !*id)
    return rh_fail(RH_ERR_CONFIG, "store_game_delete: bad arguments");

  e = prepare(ctx->db, "DELETE FROM games WHERE id = ?", &st, "delete game");
  if (!rh_err_ok(&e))
    return e;

  bind_str(st, 1, id);
  e = step_done(ctx->db, st, "delete game");
  if (!rh_err_ok(&e))
    return e;

  if (sqlite3_changes(ctx->db) == 0)
    return rh_fail(RH_ERR_NOT_FOUND, "game %s not found", id);
  return rh_ok();
}

/* ---------------------------------------------------------------- 列表查询 */

static const char *sort_column(const char *sort)
{
  if (!sort || !*sort || g_strcmp0(sort, "name") == 0)
    return "games.name";
  if (g_strcmp0(sort, "rating") == 0)
    return "games.rating";
  if (g_strcmp0(sort, "year") == 0)
    return "games.year";
  if (g_strcmp0(sort, "discs") == 0)
    return "(SELECT count(*) FROM roms r WHERE r.game_id = games.id)";
  return NULL;
}

/* FTS5 的查询串里 " 是语法字符，必须转义成 "" */
static gchar *fts_quote(const char *kw)
{
  GString *s = g_string_new("\"");

  for (const char *p = kw; *p; p++) {
    if (*p == '"')
      g_string_append(s, "\"\"");
    else
      g_string_append_c(s, *p);
  }
  g_string_append_c(s, '"');
  return g_string_free(s, FALSE);
}

static gboolean filter_uses_fts(RhCtx *ctx, const RhFilter *f)
{
  return f && f->kw && *f->kw && ctx->fts_trigram && strlen(f->kw) >= 3;
}

RhErr store_game_list(RhCtx *ctx, const RhFilter *f, GPtrArray **out)
{
  GString *sql = NULL;
  GPtrArray *binds = NULL;
  sqlite3_stmt *st = NULL;
  const char *order_col;
  GPtrArray *rows = NULL;
  RhErr e = rh_ok();
  RhFilter empty = { .multi = -1, .has_patch = -1, .sort = "name" };

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!out)
    return rh_fail(RH_ERR_CONFIG, "store_game_list: no out parameter");
  *out = NULL;
  if (!f)
    f = &empty;

  order_col = sort_column(f->sort);
  if (!order_col)
    return rh_fail(RH_ERR_CONFIG, "unknown sort key '%s'", f->sort);

  sql = g_string_new(GAME_SELECT " WHERE 1=1");
  binds = g_ptr_array_new_with_free_func(bindval_free);

  if (f->platform && *f->platform) {
    g_string_append(sql, " AND games.platform = ?");
    push_text(binds, f->platform);
  }
  if (f->genre && *f->genre) {
    g_string_append(sql, " AND games.genre = ?");
    push_text(binds, f->genre);
  }
  if (f->developer && *f->developer) {
    g_string_append(sql, " AND games.developer = ?");
    push_text(binds, f->developer);
  }
  if (f->publisher && *f->publisher) {
    g_string_append(sql, " AND games.publisher = ?");
    push_text(binds, f->publisher);
  }
  if (f->region && *f->region) {
    g_string_append(sql, " AND games.region = ?");
    push_text(binds, f->region);
  }
  if (f->patch_type && *f->patch_type) {
    g_string_append(sql, " AND games.patch_type = ?");
    push_text(binds, f->patch_type);
  }
  if (f->year_from > 0) {
    g_string_append(sql, " AND games.year >= ?");
    push_int(binds, f->year_from);
  }
  if (f->year_to > 0) {
    g_string_append(sql, " AND games.year <= ?");
    push_int(binds, f->year_to);
  }
  if (f->multi >= 0) {
    g_string_append(sql, " AND games.is_multi_disc = ?");
    push_int(binds, f->multi ? 1 : 0);
  }
  if (f->has_patch >= 0) {
    g_string_append(sql, f->has_patch ? " AND games.patch_type <> ''"
                                      : " AND games.patch_type = ''");
  }
  if (f->status && *f->status) {
    g_string_append(sql, " AND games.rom_status = ?");
    push_text(binds, f->status);
  }
  if (f->format && *f->format) {
    g_string_append(sql, " AND games.rom_format = ?");
    push_text(binds, f->format);
  }
  for (int i = 0; i < f->n_tags; i++) {
    if (!f->tags[i] || !*f->tags[i])
      continue;
    g_string_append(sql,
      " AND games.id IN (SELECT gt.game_id FROM game_tags gt"
      " JOIN tags t ON t.id = gt.tag_id WHERE t.name = ?)");
    push_text(binds, f->tags[i]);
  }

  if (f->kw && *f->kw) {
    if (filter_uses_fts(ctx, f)) {
      gchar *q = fts_quote(f->kw);
      g_string_append(sql,
        " AND games.id IN (SELECT g2.id FROM games_fts"
        " JOIN games g2 ON g2.rowid = games_fts.rowid"
        " WHERE games_fts MATCH ?)");
      push_text(binds, q);
      g_free(q);
    } else {
      g_string_append(sql,
        " AND (games.name LIKE ? OR games.game_main_name LIKE ?"
        " OR games.description LIKE ?)");
      gchar *like = g_strdup_printf("%%%s%%", f->kw);
      push_text(binds, like);
      push_text(binds, like);
      push_text(binds, like);
      g_free(like);
    }
  }

  g_string_append_printf(sql, " ORDER BY %s %s, games.id ASC", order_col,
                         f->desc ? "DESC" : "ASC");

  e = prepare(ctx->db, sql->str, &st, "list games");
  if (!rh_err_ok(&e))
    goto out;

  e = apply_binds(st, binds, "list games");
  if (!rh_err_ok(&e))
    goto out;

  rows = g_ptr_array_new_with_free_func((GDestroyNotify) rh_game_free);
  for (;;) {
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
      g_ptr_array_add(rows, row_to_game(st));
      continue;
    }
    if (rc != SQLITE_DONE)
      e = err_from_db(ctx->db, "list games");
    break;
  }

out:
  if (st)
    sqlite3_finalize(st);
  g_string_free(sql, TRUE);
  g_ptr_array_unref(binds);
  if (!rh_err_ok(&e)) {
    if (rows)
      g_ptr_array_unref(rows);
    return e;
  }
  *out = rows;
  return rh_ok();
}

RhErr store_game_count(RhCtx *ctx, gint64 *out)
{
  sqlite3_stmt *st = NULL;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!out)
    return rh_fail(RH_ERR_CONFIG, "store_game_count: bad arguments");

  if (sqlite3_prepare_v2(ctx->db, "SELECT count(*) FROM games", -1, &st, NULL)
      != SQLITE_OK)
    return err_from_db(ctx->db, "count games");

  *out = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : 0;
  sqlite3_finalize(st);
  return rh_ok();
}

/* ================================================================= 子表：ROM */

RhErr store_rom_add(RhCtx *ctx, const char *game_id, const RhRomFile *r)
{
  sqlite3_stmt *st = NULL;
  RhErr e;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!game_id || !r || !r->filename || !*r->filename)
    return rh_fail(RH_ERR_CONFIG, "store_rom_add: bad arguments");

  e = prepare(ctx->db,
    "INSERT INTO roms (game_id, filename, disc_no, size, crc32, md5, sha1, sort)"
    " VALUES (?,?,?,?,?,?,?,?)"
    " ON CONFLICT(game_id, filename) DO UPDATE SET"
    "  disc_no = excluded.disc_no, size = excluded.size,"
    "  crc32 = excluded.crc32, md5 = excluded.md5, sha1 = excluded.sha1,"
    "  sort = excluded.sort",
    &st, "add rom");
  if (!rh_err_ok(&e))
    return e;

  bind_str(st, 1, game_id);
  bind_str(st, 2, r->filename);
  bind_int_or_null(st, 3, r->disc_no);
  bind_size(st, 4, r->size);
  bind_str(st, 5, r->crc32);
  bind_str(st, 6, r->md5);
  bind_str(st, 7, r->sha1);
  sqlite3_bind_int(st, 8, r->sort);
  return step_done(ctx->db, st, "add rom");
}

RhErr store_rom_remove(RhCtx *ctx, const char *game_id, const char *filename)
{
  sqlite3_stmt *st = NULL;
  RhErr e;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!game_id || !filename)
    return rh_fail(RH_ERR_CONFIG, "store_rom_remove: bad arguments");

  e = prepare(ctx->db, "DELETE FROM roms WHERE game_id = ? AND filename = ?",
              &st, "remove rom");
  if (!rh_err_ok(&e))
    return e;

  bind_str(st, 1, game_id);
  bind_str(st, 2, filename);
  return step_done(ctx->db, st, "remove rom");
}

RhErr store_rom_list(RhCtx *ctx, const char *game_id, GPtrArray **out)
{
  sqlite3_stmt *st = NULL;
  GPtrArray *rows;
  RhErr e;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!game_id || !out)
    return rh_fail(RH_ERR_CONFIG, "store_rom_list: bad arguments");
  *out = NULL;

  e = prepare(ctx->db,
    "SELECT filename, disc_no, size, crc32, md5, sha1, sort FROM roms"
    " WHERE game_id = ? ORDER BY disc_no ASC, sort ASC, filename ASC",
    &st, "list roms");
  if (!rh_err_ok(&e))
    return e;

  bind_str(st, 1, game_id);
  rows = g_ptr_array_new_with_free_func((GDestroyNotify) rh_rom_file_free);
  for (;;) {
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
      RhRomFile *r = rh_rom_file_new();
      r->filename = col_str(st, 0);
      r->disc_no  = sqlite3_column_int(st, 1);
      r->size     = sqlite3_column_type(st, 2) == SQLITE_NULL
                  ? -1 : sqlite3_column_int64(st, 2);
      r->crc32 = col_str(st, 3);
      r->md5   = col_str(st, 4);
      r->sha1  = col_str(st, 5);
      r->sort  = sqlite3_column_int(st, 6);
      g_ptr_array_add(rows, r);
      continue;
    }
    if (rc != SQLITE_DONE) {
      e = err_from_db(ctx->db, "list roms");
      sqlite3_finalize(st);
      g_ptr_array_unref(rows);
      return e;
    }
    break;
  }
  sqlite3_finalize(st);
  *out = rows;
  return rh_ok();
}

/* ================================================================= 子表：媒体 */

RhErr store_media_add(RhCtx *ctx, const char *game_id, const char *kind,
                      const char *filename)
{
  return store_media_add_sized(ctx, game_id, kind, filename, -1, 0);
}

RhErr store_media_add_sized(RhCtx *ctx, const char *game_id, const char *kind,
                            const char *filename, gint64 size, int sort)
{
  sqlite3_stmt *st = NULL;
  RhErr e;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!game_id || !kind || !*kind || !filename || !*filename)
    return rh_fail(RH_ERR_CONFIG, "store_media_add: bad arguments");

  e = prepare(ctx->db,
    "INSERT INTO media (game_id, kind, filename, size, sort) VALUES (?,?,?,?,?)"
    " ON CONFLICT(game_id, kind, filename) DO UPDATE SET"
    "  size = excluded.size, sort = excluded.sort",
    &st, "add media");
  if (!rh_err_ok(&e))
    return e;

  bind_str(st, 1, game_id);
  bind_str(st, 2, kind);
  bind_str(st, 3, filename);
  bind_size(st, 4, size);
  sqlite3_bind_int(st, 5, sort);
  return step_done(ctx->db, st, "add media");
}

RhErr store_media_remove(RhCtx *ctx, const char *game_id, const char *kind,
                         const char *filename)
{
  sqlite3_stmt *st = NULL;
  RhErr e;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!game_id || !kind || !filename)
    return rh_fail(RH_ERR_CONFIG, "store_media_remove: bad arguments");

  e = prepare(ctx->db,
              "DELETE FROM media WHERE game_id = ? AND kind = ? AND filename = ?",
              &st, "remove media");
  if (!rh_err_ok(&e))
    return e;

  bind_str(st, 1, game_id);
  bind_str(st, 2, kind);
  bind_str(st, 3, filename);
  return step_done(ctx->db, st, "remove media");
}

RhErr store_media_list(RhCtx *ctx, const char *game_id, const char *kind,
                       GPtrArray **out)
{
  sqlite3_stmt *st = NULL;
  GPtrArray *rows;
  RhErr e;
  const char *sql_all =
    "SELECT kind, filename, size, sort FROM media WHERE game_id = ?"
    " ORDER BY kind ASC, sort ASC, filename ASC";
  const char *sql_kind =
    "SELECT kind, filename, size, sort FROM media WHERE game_id = ? AND kind = ?"
    " ORDER BY sort ASC, filename ASC";

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!game_id || !out)
    return rh_fail(RH_ERR_CONFIG, "store_media_list: bad arguments");
  *out = NULL;

  e = prepare(ctx->db, kind && *kind ? sql_kind : sql_all, &st, "list media");
  if (!rh_err_ok(&e))
    return e;

  bind_str(st, 1, game_id);
  if (kind && *kind)
    bind_str(st, 2, kind);

  rows = g_ptr_array_new_with_free_func((GDestroyNotify) rh_media_file_free);
  for (;;) {
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
      RhMediaFile *m = rh_media_file_new();
      m->kind     = col_str(st, 0);
      m->filename = col_str(st, 1);
      m->size     = sqlite3_column_type(st, 2) == SQLITE_NULL
                  ? -1 : sqlite3_column_int64(st, 2);
      m->sort     = sqlite3_column_int(st, 3);
      g_ptr_array_add(rows, m);
      continue;
    }
    if (rc != SQLITE_DONE) {
      e = err_from_db(ctx->db, "list media");
      sqlite3_finalize(st);
      g_ptr_array_unref(rows);
      return e;
    }
    break;
  }
  sqlite3_finalize(st);
  *out = rows;
  return rh_ok();
}

RhErr store_media_total_size(RhCtx *ctx, const char *game_id, gint64 *out)
{
  sqlite3_stmt *st = NULL;
  RhErr e;
  const char *sql_one =
    "SELECT COALESCE(SUM(size), 0) FROM media WHERE game_id = ?";
  const char *sql_all = "SELECT COALESCE(SUM(size), 0) FROM media";

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!out)
    return rh_fail(RH_ERR_CONFIG, "store_media_total_size: bad arguments");

  e = prepare(ctx->db, game_id && *game_id ? sql_one : sql_all, &st,
              "sum media size");
  if (!rh_err_ok(&e))
    return e;

  if (game_id && *game_id)
    bind_str(st, 1, game_id);

  *out = 0;
  if (sqlite3_step(st) == SQLITE_ROW)
    *out = sqlite3_column_int64(st, 0);
  else
    e = err_from_db(ctx->db, "sum media size");
  sqlite3_finalize(st);
  return e;
}

/* ================================================================= 标签 */

/* 规范化：去首尾空白、把连续空白折成一个空格；空串返回 NULL */
static gchar *tag_normalize(const char *raw)
{
  gchar *s, *in, *out;
  gchar *src;

  if (!raw)
    return NULL;
  src = g_strdup(raw);
  g_strstrip(src);
  if (!*src) {
    g_free(src);
    return NULL;
  }

  s = g_new0(gchar, strlen(src) + 1);
  out = s;
  for (in = src; *in; in++) {
    if (g_ascii_isspace(*in)) {
      if (out == s || *(out - 1) == ' ')
        continue;
      *out++ = ' ';
    } else {
      *out++ = *in;
    }
  }
  if (out > s && *(out - 1) == ' ')
    out--;
  *out = '\0';

  g_free(src);
  return s;
}

RhErr store_tags_set(RhCtx *ctx, const char *game_id, const char **tags,
                     int n_tags)
{
  sqlite3_stmt *st = NULL;
  RhErr e;
  gboolean owned = FALSE;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!game_id || !*game_id)
    return rh_fail(RH_ERR_CONFIG, "store_tags_set: bad arguments");

  e = tx_begin_if_needed(ctx, &owned);
  if (!rh_err_ok(&e))
    return e;

  e = prepare(ctx->db, "DELETE FROM game_tags WHERE game_id = ?", &st,
              "clear game tags");
  if (!rh_err_ok(&e))
    goto fail;
  bind_str(st, 1, game_id);
  e = step_done(ctx->db, st, "clear game tags");
  if (!rh_err_ok(&e))
    goto fail;

  for (int i = 0; i < n_tags; i++) {
    gchar *name = tag_normalize(tags ? tags[i] : NULL);
    if (!name)
      continue;

    e = prepare(ctx->db, "INSERT OR IGNORE INTO tags (name) VALUES (?)", &st,
                "insert tag");
    if (rh_err_ok(&e)) {
      bind_str(st, 1, name);
      e = step_done(ctx->db, st, "insert tag");
    }

    if (rh_err_ok(&e)) {
      e = prepare(ctx->db,
        "INSERT OR IGNORE INTO game_tags (game_id, tag_id)"
        " SELECT ?, id FROM tags WHERE name = ?", &st, "link tag");
      if (rh_err_ok(&e)) {
        bind_str(st, 1, game_id);
        bind_str(st, 2, name);
        e = step_done(ctx->db, st, "link tag");
      }
    }
    g_free(name);
    if (!rh_err_ok(&e))
      goto fail;
  }

  if (owned)
    return store_tx_commit(ctx);
  return rh_ok();

fail:
  if (owned)
    store_tx_rollback(ctx);
  return e;
}

static RhErr list_names(RhCtx *ctx, const char *sql, const char *bind1,
                        GPtrArray **out)
{
  sqlite3_stmt *st = NULL;
  GPtrArray *rows;
  RhErr e;

  *out = NULL;
  e = prepare(ctx->db, sql, &st, "list names");
  if (!rh_err_ok(&e))
    return e;
  if (bind1)
    bind_str(st, 1, bind1);

  rows = g_ptr_array_new_with_free_func(g_free);
  for (;;) {
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
      gchar *n = col_str(st, 0);
      g_ptr_array_add(rows, n ? n : g_strdup(""));
      continue;
    }
    if (rc != SQLITE_DONE) {
      e = err_from_db(ctx->db, "list names");
      sqlite3_finalize(st);
      g_ptr_array_unref(rows);
      return e;
    }
    break;
  }
  sqlite3_finalize(st);
  *out = rows;
  return rh_ok();
}

RhErr store_tag_list(RhCtx *ctx, GPtrArray **out)
{
  if (!ctx || !ctx->db)
    return err_no_db();
  if (!out)
    return rh_fail(RH_ERR_CONFIG, "store_tag_list: bad arguments");
  return list_names(ctx, "SELECT name FROM tags ORDER BY name ASC", NULL, out);
}

RhErr store_game_tags(RhCtx *ctx, const char *game_id, GPtrArray **out)
{
  if (!ctx || !ctx->db)
    return err_no_db();
  if (!game_id || !out)
    return rh_fail(RH_ERR_CONFIG, "store_game_tags: bad arguments");
  return list_names(ctx,
    "SELECT t.name FROM tags t JOIN game_tags gt ON gt.tag_id = t.id"
    " WHERE gt.game_id = ? ORDER BY t.name ASC",
    game_id, out);
}

/* ================================================================= DAT */

RhErr store_dat_begin(RhCtx *ctx, const char *platform, const char *path,
                      const char *name, const char *version, gint64 *dat_id)
{
  sqlite3_stmt *st = NULL;
  RhErr e;
  gchar *now;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!platform || !path || !dat_id)
    return rh_fail(RH_ERR_CONFIG, "store_dat_begin: bad arguments");

  now = rh_now_iso();
  e = prepare(ctx->db,
    "INSERT INTO dats (platform, path, name, version, imported_at, entry_count)"
    " VALUES (?,?,?,?,?,0)"
    " ON CONFLICT(platform, path) DO UPDATE SET"
    "  name = excluded.name, version = excluded.version,"
    "  imported_at = excluded.imported_at",
    &st, "begin dat");
  if (!rh_err_ok(&e)) {
    g_free(now);
    return e;
  }

  bind_str(st, 1, platform);
  bind_str(st, 2, path);
  bind_str(st, 3, name);
  bind_str(st, 4, version);
  bind_str(st, 5, now);
  g_free(now);

  e = step_done(ctx->db, st, "begin dat");
  if (!rh_err_ok(&e))
    return e;

  e = prepare(ctx->db,
    "SELECT id FROM dats WHERE platform = ? AND path = ?", &st, "find dat id");
  if (!rh_err_ok(&e))
    return e;
  bind_str(st, 1, platform);
  bind_str(st, 2, path);
  if (sqlite3_step(st) == SQLITE_ROW)
    *dat_id = sqlite3_column_int64(st, 0);
  else
    e = rh_fail(RH_ERR_NOT_FOUND, "dat row vanished after insert");
  sqlite3_finalize(st);
  return e;
}

RhErr store_dat_entries_add(RhCtx *ctx, gint64 dat_id, GPtrArray *entries)
{
  sqlite3_stmt *st = NULL;
  RhErr e;
  guint i;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!entries)
    return rh_fail(RH_ERR_CONFIG, "store_dat_entries_add: no entries");

  e = prepare(ctx->db,
    "INSERT INTO dat_entries (dat_id, game_name, rom_name, size, crc32, md5, sha1)"
    " VALUES (?,?,?,?,?,?,?)"
    " ON CONFLICT(dat_id, rom_name) DO UPDATE SET"
    "  game_name = excluded.game_name, size = excluded.size,"
    "  crc32 = excluded.crc32, md5 = excluded.md5, sha1 = excluded.sha1",
    &st, "add dat entries");
  if (!rh_err_ok(&e))
    return e;

  for (i = 0; i < entries->len; i++) {
    RhDatEntry *ent = g_ptr_array_index(entries, i);
    if (!ent || !ent->rom_name || !*ent->rom_name)
      continue;

    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    sqlite3_bind_int64(st, 1, dat_id);
    bind_str(st, 2, ent->game_name);
    bind_str(st, 3, ent->rom_name);
    bind_size(st, 4, ent->size);
    bind_str(st, 5, ent->crc32);
    bind_str(st, 6, ent->md5);
    bind_str(st, 7, ent->sha1);

    if (sqlite3_step(st) != SQLITE_DONE) {
      e = err_from_db(ctx->db, "add dat entries");
      goto out;
    }
  }

out:
  sqlite3_finalize(st);
  return e;
}

RhErr store_dat_finish(RhCtx *ctx, gint64 dat_id, gint64 entry_count)
{
  sqlite3_stmt *st = NULL;
  RhErr e;

  if (!ctx || !ctx->db)
    return err_no_db();

  e = prepare(ctx->db, "UPDATE dats SET entry_count = ? WHERE id = ?", &st,
              "finish dat");
  if (!rh_err_ok(&e))
    return e;

  sqlite3_bind_int64(st, 1, entry_count);
  sqlite3_bind_int64(st, 2, dat_id);
  return step_done(ctx->db, st, "finish dat");
}

RhErr store_dat_count(RhCtx *ctx, gint64 *out)
{
  sqlite3_stmt *st = NULL;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!out)
    return rh_fail(RH_ERR_CONFIG, "store_dat_count: bad arguments");

  if (sqlite3_prepare_v2(ctx->db, "SELECT count(*) FROM dat_entries", -1, &st,
                         NULL) != SQLITE_OK)
    return err_from_db(ctx->db, "count dat entries");

  *out = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : 0;
  sqlite3_finalize(st);
  return rh_ok();
}

RhErr store_dat_find_by_hash(RhCtx *ctx, const char *algo, const char *hash,
                             GPtrArray **out)
{
  sqlite3_stmt *st = NULL;
  GPtrArray *rows;
  RhErr e;
  const char *sql_crc =
    "SELECT de.dat_id, d.platform, d.name, de.game_name, de.rom_name, de.size"
    " FROM dat_entries de JOIN dats d ON d.id = de.dat_id"
    " WHERE de.crc32 = ? COLLATE NOCASE"
    " ORDER BY de.dat_id ASC, de.rom_name ASC";
  const char *sql_md5 =
    "SELECT de.dat_id, d.platform, d.name, de.game_name, de.rom_name, de.size"
    " FROM dat_entries de JOIN dats d ON d.id = de.dat_id"
    " WHERE de.md5 = ? COLLATE NOCASE"
    " ORDER BY de.dat_id ASC, de.rom_name ASC";
  const char *sql_sha1 =
    "SELECT de.dat_id, d.platform, d.name, de.game_name, de.rom_name, de.size"
    " FROM dat_entries de JOIN dats d ON d.id = de.dat_id"
    " WHERE de.sha1 = ? COLLATE NOCASE"
    " ORDER BY de.dat_id ASC, de.rom_name ASC";

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!algo || !hash || !out)
    return rh_fail(RH_ERR_CONFIG, "store_dat_find_by_hash: bad arguments");
  *out = NULL;

  if (g_ascii_strcasecmp(algo, "crc32") == 0)
    e = prepare(ctx->db, sql_crc, &st, "find by crc32");
  else if (g_ascii_strcasecmp(algo, "md5") == 0)
    e = prepare(ctx->db, sql_md5, &st, "find by md5");
  else if (g_ascii_strcasecmp(algo, "sha1") == 0)
    e = prepare(ctx->db, sql_sha1, &st, "find by sha1");
  else
    return rh_fail(RH_ERR_UNSUPPORTED, "unknown hash algorithm '%s'", algo);

  if (!rh_err_ok(&e))
    return e;

  bind_str(st, 1, hash);
  rows = g_ptr_array_new_with_free_func((GDestroyNotify) rh_dat_match_free);
  for (;;) {
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
      RhDatMatch *m = rh_dat_match_new();
      m->dat_id    = sqlite3_column_int64(st, 0);
      m->platform  = col_str(st, 1);
      m->dat_name  = col_str(st, 2);
      m->game_name = col_str(st, 3);
      m->rom_name  = col_str(st, 4);
      m->size      = sqlite3_column_type(st, 5) == SQLITE_NULL
                   ? -1 : sqlite3_column_int64(st, 5);
      g_ptr_array_add(rows, m);
      continue;
    }
    if (rc != SQLITE_DONE) {
      e = err_from_db(ctx->db, "find by hash");
      sqlite3_finalize(st);
      g_ptr_array_unref(rows);
      return e;
    }
    break;
  }
  sqlite3_finalize(st);
  *out = rows;
  return rh_ok();
}

/* ================================================================= 作业 */

RhErr store_job_create(RhCtx *ctx, const char *kind, int total, gint64 *job_id)
{
  sqlite3_stmt *st = NULL;
  RhErr e;
  gchar *now;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!kind || !*kind || !job_id)
    return rh_fail(RH_ERR_CONFIG, "store_job_create: bad arguments");

  now = rh_now_iso();
  e = prepare(ctx->db,
    "INSERT INTO jobs (kind, state, total, done, created_at)"
    " VALUES (?, 'queued', ?, 0, ?)", &st, "create job");
  if (!rh_err_ok(&e)) {
    g_free(now);
    return e;
  }

  bind_str(st, 1, kind);
  sqlite3_bind_int(st, 2, total < 0 ? 0 : total);
  bind_str(st, 3, now);
  g_free(now);

  e = step_done(ctx->db, st, "create job");
  if (rh_err_ok(&e))
    *job_id = (gint64) sqlite3_last_insert_rowid(ctx->db);
  return e;
}

RhErr store_job_update(RhCtx *ctx, gint64 job_id, const char *state, int done,
                       const char *err)
{
  sqlite3_stmt *st = NULL;
  RhErr e;
  gboolean finished;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!state || !*state)
    return rh_fail(RH_ERR_CONFIG, "store_job_update: state is required");

  finished = (g_strcmp0(state, "done") == 0 || g_strcmp0(state, "failed") == 0 ||
              g_strcmp0(state, "cancelled") == 0);

  e = prepare(ctx->db,
    "UPDATE jobs SET state = ?, done = ?, error = ?, finished_at = ?"
    " WHERE id = ?", &st, "update job");
  if (!rh_err_ok(&e))
    return e;

  bind_str(st, 1, state);
  sqlite3_bind_int(st, 2, done < 0 ? 0 : done);
  bind_str(st, 3, err);
  if (finished) {
    gchar *now = rh_now_iso();
    bind_str(st, 4, now);
    g_free(now);
  } else {
    sqlite3_bind_null(st, 4);
  }
  sqlite3_bind_int64(st, 5, job_id);
  return step_done(ctx->db, st, "update job");
}

RhErr store_job_get(RhCtx *ctx, gint64 job_id, RhJob **out)
{
  sqlite3_stmt *st = NULL;
  RhErr e;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!out)
    return rh_fail(RH_ERR_CONFIG, "store_job_get: bad arguments");
  *out = NULL;

  e = prepare(ctx->db,
    "SELECT id, kind, state, total, done, created_at, finished_at, error"
    " FROM jobs WHERE id = ?", &st, "get job");
  if (!rh_err_ok(&e))
    return e;

  sqlite3_bind_int64(st, 1, job_id);
  if (sqlite3_step(st) == SQLITE_ROW) {
    RhJob *j = rh_job_new();
    j->id         = sqlite3_column_int64(st, 0);
    j->kind       = col_str(st, 1);
    j->state      = col_str(st, 2);
    j->total      = sqlite3_column_int(st, 3);
    j->done       = sqlite3_column_int(st, 4);
    j->created_at = col_str(st, 5);
    j->finished_at = col_str(st, 6);
    j->error      = col_str(st, 7);
    *out = j;
    e = rh_ok();
  } else {
    e = rh_fail(RH_ERR_NOT_FOUND, "job %lld not found", (long long) job_id);
  }
  sqlite3_finalize(st);
  return e;
}

RhErr store_job_list_unfinished(RhCtx *ctx, GPtrArray **out)
{
  sqlite3_stmt *st = NULL;
  GPtrArray *rows;
  RhErr e;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!out)
    return rh_fail(RH_ERR_CONFIG, "store_job_list_unfinished: bad arguments");
  *out = NULL;

  e = prepare(ctx->db,
    "SELECT id, kind, state, total, done, created_at, finished_at, error"
    " FROM jobs WHERE state IN ('queued','running','paused')"
    " ORDER BY id ASC", &st, "list unfinished jobs");
  if (!rh_err_ok(&e))
    return e;

  rows = g_ptr_array_new_with_free_func((GDestroyNotify) rh_job_free);
  for (;;) {
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
      RhJob *j = rh_job_new();
      j->id         = sqlite3_column_int64(st, 0);
      j->kind       = col_str(st, 1);
      j->state      = col_str(st, 2);
      j->total      = sqlite3_column_int(st, 3);
      j->done       = sqlite3_column_int(st, 4);
      j->created_at = col_str(st, 5);
      j->finished_at = col_str(st, 6);
      j->error      = col_str(st, 7);
      g_ptr_array_add(rows, j);
      continue;
    }
    if (rc != SQLITE_DONE) {
      e = err_from_db(ctx->db, "list unfinished jobs");
      sqlite3_finalize(st);
      g_ptr_array_unref(rows);
      return e;
    }
    break;
  }
  sqlite3_finalize(st);
  *out = rows;
  return rh_ok();
}

RhErr store_job_item_add(RhCtx *ctx, RhJobItem *it)
{
  sqlite3_stmt *st = NULL;
  RhErr e;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!it || !it->src || !*it->src)
    return rh_fail(RH_ERR_CONFIG, "store_job_item_add: bad arguments");

  e = prepare(ctx->db,
    "INSERT INTO job_items (job_id, seq, src, action, platform, target_id,"
    " state, message) VALUES (?,?,?,?,?,?,?,?)",
    &st, "add job item");
  if (!rh_err_ok(&e))
    return e;

  sqlite3_bind_int64(st, 1, it->job_id);
  sqlite3_bind_int(st, 2, it->seq);
  bind_str(st, 3, it->src);
  bind_str(st, 4, it->action);
  bind_str(st, 5, it->platform);
  bind_str(st, 6, it->target_id);
  bind_str(st, 7, it->state && *it->state ? it->state : "pending");
  bind_str(st, 8, it->message);
  return step_done(ctx->db, st, "add job item");
}

RhErr store_job_item_update(RhCtx *ctx, gint64 job_id, int seq,
                            const char *state, const char *message)
{
  sqlite3_stmt *st = NULL;
  RhErr e;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!state || !*state)
    return rh_fail(RH_ERR_CONFIG, "store_job_item_update: state is required");

  e = prepare(ctx->db,
    "UPDATE job_items SET state = ?, message = ? WHERE job_id = ? AND seq = ?",
    &st, "update job item");
  if (!rh_err_ok(&e))
    return e;

  bind_str(st, 1, state);
  bind_str(st, 2, message);
  sqlite3_bind_int64(st, 3, job_id);
  sqlite3_bind_int(st, 4, seq);
  return step_done(ctx->db, st, "update job item");
}

RhErr store_job_items_list(RhCtx *ctx, gint64 job_id, GPtrArray **out)
{
  sqlite3_stmt *st = NULL;
  GPtrArray *rows;
  RhErr e;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!out)
    return rh_fail(RH_ERR_CONFIG, "store_job_items_list: bad arguments");
  *out = NULL;

  e = prepare(ctx->db,
    "SELECT job_id, seq, src, action, platform, target_id, state, message"
    " FROM job_items WHERE job_id = ? ORDER BY seq ASC", &st, "list job items");
  if (!rh_err_ok(&e))
    return e;

  sqlite3_bind_int64(st, 1, job_id);
  rows = g_ptr_array_new_with_free_func((GDestroyNotify) rh_job_item_free);
  for (;;) {
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
      RhJobItem *it = rh_job_item_new();
      g_free(it->state);
      it->job_id    = sqlite3_column_int64(st, 0);
      it->seq       = sqlite3_column_int(st, 1);
      it->src       = col_str(st, 2);
      it->action    = col_str(st, 3);
      it->platform  = col_str(st, 4);
      it->target_id = col_str(st, 5);
      it->state     = col_str(st, 6);
      it->message   = col_str(st, 7);
      g_ptr_array_add(rows, it);
      continue;
    }
    if (rc != SQLITE_DONE) {
      e = err_from_db(ctx->db, "list job items");
      sqlite3_finalize(st);
      g_ptr_array_unref(rows);
      return e;
    }
    break;
  }
  sqlite3_finalize(st);
  *out = rows;
  return rh_ok();
}

/* ================================================================= 检索 */

RhErr store_search_fts(RhCtx *ctx, const char *kw, GPtrArray **out)
{
  RhFilter f = { .multi = -1, .has_patch = -1, .sort = "name", .kw = kw };

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!out)
    return rh_fail(RH_ERR_CONFIG, "store_search_fts: bad arguments");
  return store_game_list(ctx, &f, out);
}

/* ================================================================= 统计 */

RhErr store_stats(RhCtx *ctx, RhStats *out, GPtrArray **by_platform)
{
  static const char *one =
    "SELECT (SELECT count(*) FROM games),"
    " (SELECT count(DISTINCT platform) FROM games),"
    " (SELECT count(*) FROM roms),"
    " (SELECT count(*) FROM media),"
    " (SELECT COALESCE(sum(CASE WHEN size > 0 THEN size ELSE 0 END),0) FROM media),"
    " (SELECT count(*) FROM dat_entries)";
  static const char *per_plat =
    "SELECT g.platform, count(DISTINCT g.id),"
    " COALESCE(sum(CASE WHEN m.size > 0 THEN m.size ELSE 0 END),0)"
    " FROM games g LEFT JOIN media m ON m.game_id = g.id"
    " GROUP BY g.platform ORDER BY g.platform";
  sqlite3_stmt *st = NULL;
  GPtrArray *rows = NULL;
  RhErr e;

  if (!ctx || !ctx->db)
    return err_no_db();
  if (!out || !by_platform)
    return rh_fail(RH_ERR_CONFIG, "store_stats: bad arguments");
  *by_platform = NULL;
  memset(out, 0, sizeof(*out));

  if (sqlite3_prepare_v2(ctx->db, one, -1, &st, NULL) != SQLITE_OK)
    return err_from_db(ctx->db, "stats totals");
  if (sqlite3_step(st) == SQLITE_ROW) {
    out->games = sqlite3_column_int64(st, 0);
    out->platforms = sqlite3_column_int64(st, 1);
    out->roms = sqlite3_column_int64(st, 2);
    out->media_files = sqlite3_column_int64(st, 3);
    out->media_bytes = sqlite3_column_int64(st, 4);
    out->dat_entries = sqlite3_column_int64(st, 5);
  }
  sqlite3_finalize(st);
  st = NULL;

  if (sqlite3_prepare_v2(ctx->db, per_plat, -1, &st, NULL) != SQLITE_OK)
    return err_from_db(ctx->db, "stats by platform");

  rows = g_ptr_array_new_with_free_func((GDestroyNotify) rh_platform_stat_free);
  for (;;) {
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
      RhPlatformStat *p = rh_platform_stat_new();
      p->platform = g_strdup((const char *) sqlite3_column_text(st, 0));
      p->games = sqlite3_column_int64(st, 1);
      p->media_bytes = sqlite3_column_int64(st, 2);
      g_ptr_array_add(rows, p);
      continue;
    }
    if (rc != SQLITE_DONE)
      e = err_from_db(ctx->db, "stats by platform");
    break;
  }
  sqlite3_finalize(st);
  if (!rh_err_ok(&e)) {
    g_ptr_array_unref(rows);
    return e;
  }
  *by_platform = rows;
  return rh_ok();
}
