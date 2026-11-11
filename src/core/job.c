/*
 * job.c — 作业生命周期（薄封装，SQL 全部在 store.c）
 */
#include "job.h"

#include <string.h>

#include "store.h"

gint64 rh_job_begin(RhCtx *ctx, const char *kind, GPtrArray *plan_items)
{
  gint64 job_id = 0;
  RhErr e;

  e = store_job_create(ctx, kind, plan_items ? (int) plan_items->len : 0, &job_id);
  if (!rh_err_ok(&e))
    return 0;

  for (guint i = 0; plan_items && i < plan_items->len; i++) {
    RhJobItem *it = plan_items->pdata[i];

    it->job_id = job_id;
    it->seq = (int) i + 1;
    it->state = g_strdup("pending");
    e = store_job_item_add(ctx, it);
    if (!rh_err_ok(&e)) {
      rh_job_finish(ctx, job_id, "failed", e.msg);
      return 0;
    }
  }
  return job_id;
}

void rh_job_finish(RhCtx *ctx, gint64 job_id, const char *state, const char *err)
{
  int done = 0;
  GPtrArray *items = NULL;

  if (job_id <= 0)
    return;
  {
    RhErr e = store_job_items_list(ctx, job_id, &items);

    if (rh_err_ok(&e) && items) {
      for (guint i = 0; i < items->len; i++) {
        RhJobItem *it = items->pdata[i];

        if (it->state && strcmp(it->state, "done") == 0)
          done++;
      }
      g_ptr_array_unref(items);
    }
  }
  (void) store_job_update(ctx, job_id, state, done, err);
}

gboolean rh_job_cancelled(RhCtx *ctx)
{
  return ctx && g_atomic_int_get(&ctx->cancel_flag) != 0;
}

void rh_job_cancel(RhCtx *ctx)
{
  if (ctx)
    g_atomic_int_set(&ctx->cancel_flag, 1);
}

RhErr rh_job_items(RhCtx *ctx, gint64 job_id, GPtrArray **out)
{
  return store_job_items_list(ctx, job_id, out);
}

RhErr rh_job_unfinished(RhCtx *ctx, GPtrArray **out)
{
  return store_job_list_unfinished(ctx, out);
}
