/*
 * rh-ctx.h — 上下文对象
 *
 * 硬约束：核心库不持全局可变状态，一切经 RhCtx 传递（ARCHITECTURE §1.1）。
 */
#pragma once

#include <glib.h>

#include "rh-config.h"
#include "rh-err.h"
#include "rh-ui.h"

typedef struct sqlite3 sqlite3;   /* 前向声明，避免把 sqlite3.h 漏到前端 */

typedef struct RhCtx {
  RhConfig  *config;
  sqlite3   *db;                 /* store_open 后非空 */
  const RhUi *ui;                /* 可为 NULL = 无界面运行 */
  gchar     *db_path;
  gchar     *cache_dir;
  gboolean   in_tx;              /* store_tx_begin 之后为 TRUE */
  gboolean   fts_trigram;        /* 库是否支持 trigram 分词 */
  gint       cancel_flag;        /* 作业线程只写这一处（atomic_int 语义） */
  gpointer   user_data;          /* 前端私有数据，核心不动 */
} RhCtx;

RhCtx *rh_ctx_new(void);
void   rh_ctx_free(RhCtx *ctx);

void   rh_ctx_set_ui(RhCtx *ctx, const RhUi *ui);

/* 兼容旧前端：没有 ui 时静默丢弃，不会打印 */
void   rh_log(RhCtx *ctx, RhLogLevel lv, const char *fmt, ...)
       G_GNUC_PRINTF(3, 4);
void   rh_progress(RhCtx *ctx, double pct, const char *label);

/* 按配置解析出实际库路径：store_path 为空时用 <ROM_ROOT>/romhamster.db */
gchar *rh_ctx_resolve_db_path(RhCtx *ctx, const char *rom_root);
