/*
 * manage.h — 业务动作（ARCHITECTURE §5：编辑/删除/媒体/完整性/统计）
 *
 * 菜单与交互留给前端；这里只做事：改一条记录、删一个游戏、挂/摘媒体、
 * 检查库与磁盘是否一致。不做检索展示。
 */
#pragma once

#include <glib.h>

#include "rh-ctx.h"
#include "rh-err.h"
#include "rh-records.h"

/* 部分更新：set_* = FALSE 表示该字段保持原值。字符串为 NULL 且 set_* = TRUE
 * 表示清空。rating 用 -1 表示未知。 */
typedef struct {
  const char *name;
  const char *game_main_name;
  const char *description;
  const char *developer;
  const char *publisher;
  const char *genre;
  const char *region;
  const char *rom_status;
  const char *patch_type;
  int         year;
  int         players;
  double      rating;
  gboolean    set_name;
  gboolean    set_game_main_name;
  gboolean    set_description;
  gboolean    set_developer;
  gboolean    set_publisher;
  gboolean    set_genre;
  gboolean    set_region;
  gboolean    set_rom_status;
  gboolean    set_patch_type;
  gboolean    set_year;
  gboolean    set_players;
  gboolean    set_rating;
  gboolean    set_tags;
  const char **tags;
  int         n_tags;
} RhGamePatch;

RhErr rh_manage_update(RhCtx *ctx, const char *game_id, const RhGamePatch *p);

/* delete_files = TRUE 时连同游戏目录一起删除（目录必须位于 ROM_ROOT 内） */
RhErr rh_manage_delete(RhCtx *ctx, const char *game_id, gboolean delete_files);

/* 把 src 复制进 <游戏目录>/<kind>/<basename>，并写一条 media 记录。
 * kind 只接受 media 表 CHECK 允许的 7 种。 */
RhErr rh_manage_add_media(RhCtx *ctx, const char *game_id, const char *kind,
                          const char *src_path);

RhErr rh_manage_remove_media(RhCtx *ctx, const char *game_id, const char *kind,
                             const char *filename, gboolean delete_file);

/* 完整性检查结果 */
typedef struct {
  gint64 games_checked;
  gint64 missing_dirs;
  gint64 missing_roms;
  gint64 missing_media;
  GPtrArray *details;      /* GPtrArray<gchar*>，每条 "id: 原因" */
} RhIntegrity;

RhErr rh_manage_integrity(RhCtx *ctx, RhIntegrity *out);
void  rh_integrity_clear(RhIntegrity *r);
