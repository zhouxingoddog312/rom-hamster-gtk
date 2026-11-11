/*
 * library.h — 整库导入（ES / ES-DE gamelist.xml、Pegasus metadata）
 */
#pragma once

#include <glib.h>

#include "import.h"
#include "platform.h"
#include "rh-ctx.h"
#include "rh-err.h"

/* 在 dir 下最多 3 层找清单；*manifests = GPtrArray<gchar*>，可能为空 */
RhErr rh_library_detect(const char *dir, GPtrArray **manifests);

/* 整库导入。返回统计；单条失败不中断整体 */
RhErr rh_library_import(RhCtx *ctx, RhPlatformRules *rules, const char *dir,
                       const RhImportOpts *opts, RhImportReport **out);
