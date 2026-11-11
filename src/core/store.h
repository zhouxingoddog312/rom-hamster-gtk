/*
 * store.h — 唯一持有 SQL 的一层（ARCHITECTURE §4 / §5.1）
 *
 * 全部返回 RhErr；不使用 assert 处理外部输入。所有出参用**指针的指针**，
 * 所有权都在注释里写明：store_* 分配、调用方用对应的 rh_*_free 释放。
 * 与 §4 原文的两处差异已在 docs/m1-verification.md 记录：
 *   - store_game_get / store_*_list 返回分配出来的对象，而不是调用方预置结构体；
 *   - store_dat_import_file 拆成 begin/entries_add/finish，XML 解析属 dat.c（§5.1）。
 */
#pragma once

#include <glib.h>

#include "rh-ctx.h"
#include "rh-err.h"
#include "rh-records.h"

#define STORE_OPEN_CREATE   (1 << 0)
#define STORE_OPEN_READONLY (1 << 1)

/* ----------------------------------------------------------- 生命周期与事务 */

RhErr store_open(RhCtx *ctx, const char *db_path, int flags);
void  store_close(RhCtx *ctx);

/* 依 PRAGMA user_version 逐级升级；store_open 内部已调用 */
RhErr store_migrate(RhCtx *ctx);

RhErr store_tx_begin(RhCtx *ctx);     /* BEGIN IMMEDIATE：拿写锁 */
RhErr store_tx_commit(RhCtx *ctx);
RhErr store_tx_rollback(RhCtx *ctx);

/* ----------------------------------------------------------- 游戏记录 */

/* *out 由本函数分配，调用方 rh_game_free()。未找到 → RH_ERR_NOT_FOUND */
RhErr store_game_get(RhCtx *ctx, const char *id, RhGame **out);

/* id 为空时按 dir_path 自动生成；created_at 保留首次写入值 */
RhErr store_game_upsert(RhCtx *ctx, const RhGame *g);

RhErr store_game_delete(RhCtx *ctx, const char *id);

/* *out = GPtrArray<RhGame*>，free_func 已设好，调用方 g_ptr_array_unref() */
RhErr store_game_list(RhCtx *ctx, const RhFilter *f, GPtrArray **out);

/* 统计（M3）：库级汇总 + 按平台分布（*by_platform = GPtrArray<RhPlatformStat*>） */
RhErr store_stats(RhCtx *ctx, RhStats *out, GPtrArray **by_platform);

RhErr store_game_count(RhCtx *ctx, gint64 *out);

/* ----------------------------------------------------------- 子表：ROM */

RhErr store_rom_add(RhCtx *ctx, const char *game_id, const RhRomFile *r);
RhErr store_rom_remove(RhCtx *ctx, const char *game_id, const char *filename);

/* *out = GPtrArray<RhRomFile*> */
RhErr store_rom_list(RhCtx *ctx, const char *game_id, GPtrArray **out);

/* ----------------------------------------------------------- 子表：媒体 */

/* 只落元数据；size 未知时为 NULL。需要 size 请用 …_sized 版本 */
RhErr store_media_add(RhCtx *ctx, const char *game_id, const char *kind,
                      const char *filename);
RhErr store_media_add_sized(RhCtx *ctx, const char *game_id, const char *kind,
                            const char *filename, gint64 size, int sort);
RhErr store_media_remove(RhCtx *ctx, const char *game_id, const char *kind,
                         const char *filename);

/* kind == NULL 表示全部类型。*out = GPtrArray<RhMediaFile*> */
RhErr store_media_list(RhCtx *ctx, const char *game_id, const char *kind,
                       GPtrArray **out);

/* 媒体总字节数（stats 用 SUM(size)，不逐个 stat，见 ARCHITECTURE §8.3 坑三） */
RhErr store_media_total_size(RhCtx *ctx, const char *game_id, gint64 *out);

/* ----------------------------------------------------------- 标签 */

/* 整体替换该游戏的标签集合；tags 为空/NULL 时清空。事务由本函数自理 */
RhErr store_tags_set(RhCtx *ctx, const char *game_id, const char **tags,
                     int n_tags);

/* *out = GPtrArray<gchar*>（标签名，调用方 g_ptr_array_unref） */
RhErr store_tag_list(RhCtx *ctx, GPtrArray **out);
RhErr store_game_tags(RhCtx *ctx, const char *game_id, GPtrArray **out);

/* ----------------------------------------------------------- DAT */

/*
 * DAT 的 XML 解析在 dat.c（§5.1 边界表），store 只负责落库与按哈希查。
 * 典型用法：begin → entries_add（可多次）→ finish。
 */
RhErr store_dat_begin(RhCtx *ctx, const char *platform, const char *path,
                      const char *name, const char *version, gint64 *dat_id);
RhErr store_dat_entries_add(RhCtx *ctx, gint64 dat_id, GPtrArray *entries);
RhErr store_dat_finish(RhCtx *ctx, gint64 dat_id, gint64 entry_count);

RhErr store_dat_count(RhCtx *ctx, gint64 *out);

/* algo: "crc32" / "md5" / "sha1"。*out = GPtrArray<RhDatMatch*> */
RhErr store_dat_find_by_hash(RhCtx *ctx, const char *algo, const char *hash,
                             GPtrArray **out);

/* ----------------------------------------------------------- 作业 */

RhErr store_job_create(RhCtx *ctx, const char *kind, int total, gint64 *job_id);
RhErr store_job_update(RhCtx *ctx, gint64 job_id, const char *state, int done,
                       const char *err);
RhErr store_job_get(RhCtx *ctx, gint64 job_id, RhJob **out);
RhErr store_job_list_unfinished(RhCtx *ctx, GPtrArray **out);

RhErr store_job_item_add(RhCtx *ctx, RhJobItem *it);
RhErr store_job_item_update(RhCtx *ctx, gint64 job_id, int seq,
                            const char *state, const char *message);
RhErr store_job_items_list(RhCtx *ctx, gint64 job_id, GPtrArray **out);

/* ----------------------------------------------------------- 检索 */

/* 关键字检索；trigram 可用时走 FTS5 MATCH，否则退化为 LIKE（§3.3） */
RhErr store_search_fts(RhCtx *ctx, const char *kw, GPtrArray **out);
