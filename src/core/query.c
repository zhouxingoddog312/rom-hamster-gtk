/*
 * query.c — 只读检索（ARCHITECTURE §5.1：绝不写库）
 */
#include "query.h"

#include <string.h>

#include "store.h"

gboolean rh_query_sort_valid(const char *sort)
{
  if (!sort)
    return FALSE;
  return g_ascii_strcasecmp(sort, "name") == 0
      || g_ascii_strcasecmp(sort, "rating") == 0
      || g_ascii_strcasecmp(sort, "year") == 0
      || g_ascii_strcasecmp(sort, "discs") == 0;
}

RhErr rh_query_list(RhCtx *ctx, const RhFilter *f, GPtrArray **out)
{
  if (!ctx || !ctx->db)
    return rh_fail(RH_ERR_DB, "query: database is not open");
  if (!out)
    return rh_fail(RH_ERR_CONFIG, "query: bad arguments");
  if (f && f->sort && !rh_query_sort_valid(f->sort))
    return rh_fail(RH_ERR_CONFIG, "query: unknown sort key '%s'", f->sort);
  return store_game_list(ctx, f, out);
}

RhErr rh_query_stats(RhCtx *ctx, RhStats *out, GPtrArray **by_platform)
{
  if (!ctx || !ctx->db)
    return rh_fail(RH_ERR_DB, "query: database is not open");
  return store_stats(ctx, out, by_platform);
}
