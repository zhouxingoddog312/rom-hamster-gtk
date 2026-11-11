/*
 * import.h — 导入编排（ARCHITECTURE §6.2 两阶段、§11 写入一致性、§14 M2）
 *
 * 第一阶段（扫描）在主线程完成全部交互并把决策固化进计划；
 * 第二阶段（执行）不发起任何交互，只搬文件 + 落库，可安全放进工作线程。
 */
#pragma once

#include <glib.h>

#include "platform.h"
#include "rh-ctx.h"
#include "rh-err.h"

typedef struct {
  const char *rom_root;          /* NULL = 用配置 ROM_ROOT */
  gboolean    non_interactive;   /* -y：所有选择取默认 */
  gboolean    overwrite;         /* 目标存在且不同时覆盖 */
  gboolean    dry_run;           /* 只扫描，不搬文件、不写库 */
} RhImportOpts;

typedef struct {
  int    planned;
  int    imported;      /* ROM 条目成功 */
  int    media;         /* 媒体/补丁条目成功 */
  int    skipped;
  int    failed;
  gint64 job_id;
} RhImportReport;

/* inputs = GPtrArray<gchar*>（文件或目录的绝对路径） */
RhErr rh_import_run(RhCtx *ctx, RhPlatformRules *rules, GPtrArray *inputs,
                    const RhImportOpts *opts, RhImportReport **out);
