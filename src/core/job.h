/*
 * job.h — 导入作业的创建/进度/续做（ARCHITECTURE §6.2 / §11）
 *
 * 作业状态全部落库（jobs / job_items），进程被杀后能凭它续做；
 * 核心不打印，进度只走 ui->progress / ui->log。
 */
#pragma once

#include <glib.h>

#include "rh-ctx.h"
#include "rh-records.h"
#include "rh-err.h"

/* 建作业并分配 items（pending）。返回 job_id（0 = 失败） */
gint64 rh_job_begin(RhCtx *ctx, const char *kind, GPtrArray *plan_items);

/* 结束作业；state = done / failed / cancelled */
void rh_job_finish(RhCtx *ctx, gint64 job_id, const char *state, const char *err);

/* 取消标志（atomic 读） */
gboolean rh_job_cancelled(RhCtx *ctx);
void     rh_job_cancel(RhCtx *ctx);

/* 读取某作业的 job_items；*out = GPtrArray<RhJobItem*> */
RhErr rh_job_items(RhCtx *ctx, gint64 job_id, GPtrArray **out);

/* 未完成作业（启动时提示续做） */
RhErr rh_job_unfinished(RhCtx *ctx, GPtrArray **out);
