/*
 * store.h — 唯一持有 SQL 的一层（ARCHITECTURE §4）
 *
 * 全部返回 RhErr；不使用 assert 处理外部输入。M0 只实现生命周期、事务与
 * 一条计数查询，M1 补齐 §4 的其余 API。
 */
#pragma once

#include <glib.h>

#include "rh-ctx.h"
#include "rh-err.h"

#define STORE_OPEN_CREATE   (1 << 0)
#define STORE_OPEN_READONLY (1 << 1)

/* 打开（必要时建库并建 schema） */
RhErr store_open(RhCtx *ctx, const char *db_path, int flags);
void  store_close(RhCtx *ctx);

/* 依 PRAGMA user_version 逐级升级；store_open 内部已调用 */
RhErr store_migrate(RhCtx *ctx);

/* 事务（BEGIN IMMEDIATE：拿写锁，避免先读后写被别的实例插进来） */
RhErr store_tx_begin(RhCtx *ctx);
RhErr store_tx_commit(RhCtx *ctx);
RhErr store_tx_rollback(RhCtx *ctx);

/* 计数：M0 用来验证"库真的建起来了" */
RhErr store_game_count(RhCtx *ctx, gint64 *out);
