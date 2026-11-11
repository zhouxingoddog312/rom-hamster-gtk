/*
 * query.h — 只读检索（ARCHITECTURE §5 / §5.1）
 *
 * 13 维筛选 + 4 个排序键。筛选条件最终由 store_game_list 落成 SQL，
 * 本模块只负责校验与转发，**绝不写库**，也绝不出现 SQL 语句。
 */
#pragma once

#include <glib.h>

#include "rh-ctx.h"
#include "rh-err.h"
#include "rh-records.h"

/* 4 个排序键：name / rating / year / discs；大小写不敏感。
 * 非法键返回 FALSE。 */
gboolean rh_query_sort_valid(const char *sort);

/* 只读列表；*out = GPtrArray<RhGame*>，调用方 g_ptr_array_unref() */
RhErr rh_query_list(RhCtx *ctx, const RhFilter *f, GPtrArray **out);

/* 统计：库级汇总 + 按平台分布（调用方 free 两个出参） */
RhErr rh_query_stats(RhCtx *ctx, RhStats *out, GPtrArray **by_platform);
