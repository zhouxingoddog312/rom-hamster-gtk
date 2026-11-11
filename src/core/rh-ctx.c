#include "rh-ctx.h"

#include <stdarg.h>

RhCtx *rh_ctx_new(void)
{
  RhCtx *ctx = g_new0(RhCtx, 1);

  ctx->config = rh_config_new();
  rh_config_load_standard(ctx->config);

  ctx->cache_dir = g_build_filename(g_get_user_cache_dir(), "rom-hamster", NULL);
  g_mkdir_with_parents(ctx->cache_dir, 0755);

  return ctx;
}

void rh_ctx_free(RhCtx *ctx)
{
  if (!ctx)
    return;
  /* store 的关闭由调用方负责：rh_ctx_free 不替它做决定 */
  rh_config_free(ctx->config);
  g_free(ctx->db_path);
  g_free(ctx->cache_dir);
  g_free(ctx);
}

void rh_ctx_set_ui(RhCtx *ctx, const RhUi *ui)
{
  if (ctx)
    ctx->ui = ui;
}

void rh_log(RhCtx *ctx, RhLogLevel lv, const char *fmt, ...)
{
  va_list ap;
  gchar  *msg;

  if (!ctx || !ctx->ui || !ctx->ui->log)
    return;                       /* 无 ui：静默，绝不打印 */

  va_start(ap, fmt);
  msg = g_strdup_vprintf(fmt, ap);
  va_end(ap);

  ctx->ui->log(ctx->ui->userdata, lv, msg);
  g_free(msg);
}

void rh_progress(RhCtx *ctx, double pct, const char *label)
{
  if (!ctx || !ctx->ui || !ctx->ui->progress)
    return;
  ctx->ui->progress(ctx->ui->userdata, pct, label);
}

gchar *rh_ctx_resolve_db_path(RhCtx *ctx, const char *rom_root)
{
  const gchar *configured = rh_config_get(ctx->config, "store_path");

  if (configured && *configured)
    return g_strdup(configured);

  if (rom_root && *rom_root)
    return g_build_filename(rom_root, "romhamster.db", NULL);

  return NULL;   /* 还没选目录：由前端走首次运行引导 */
}
