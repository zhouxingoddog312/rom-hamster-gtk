/*
 * game-model.h — 主窗口列表的 GListModel
 *
 * RhGame 是只读记录，不能直接当 GListModel 项；这里转成轻量 RhGameRow。
 * 10k 条记录时只保留展示所需的 6 个字段，不复制描述等大文本。
 */
#pragma once

#include <gio/gio.h>
#include <glib-object.h>

#include "rh-records.h"

G_BEGIN_DECLS

#define RH_TYPE_GAME_ROW (rh_game_row_get_type())
G_DECLARE_FINAL_TYPE(RhGameRow, rh_game_row, RH, GAME_ROW, GObject)

const char *rh_game_row_id(RhGameRow *r);
const char *rh_game_row_name(RhGameRow *r);
const char *rh_game_row_platform(RhGameRow *r);
const char *rh_game_row_year(RhGameRow *r);
const char *rh_game_row_genre(RhGameRow *r);
const char *rh_game_row_rating(RhGameRow *r);

#define RH_TYPE_GAME_LIST_MODEL (rh_game_list_model_get_type())
G_DECLARE_FINAL_TYPE(RhGameListModel, rh_game_list_model, RH,
                     GAME_LIST_MODEL, GObject)

RhGameListModel *rh_game_list_model_new(void);

/* 接管 games 的所有权（GPtrArray<RhGame*>），转换后释放。 */
void rh_game_list_model_set_games(RhGameListModel *m, GPtrArray *games);

guint rh_game_list_model_get_count(RhGameListModel *m);

G_END_DECLS
