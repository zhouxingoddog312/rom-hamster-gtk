/*
 * export.h — 前端导出（ARCHITECTURE §5 / §7.2）
 *
 * 一份 data/frontends/<id>.conf 同时描述清单模板、媒体落位与导入字段。
 * 模板语法是闭集：%var%、%var:xml|json|text%、%if:var%…%else%…%endif%、
 * %for:kind%…%endfor%。未定义变量在加载期报 "文件:行"，绝不静默留空。
 */
#pragma once

#include <glib.h>

#include "rh-ctx.h"
#include "rh-err.h"
#include "rh-records.h"

typedef struct RhFrontend RhFrontend;

/* 加载并预编译一份前端规格；失败时 err->msg 带 "文件:行" */
RhErr rh_frontend_load(const char *path, RhFrontend **out);
void  rh_frontend_free(RhFrontend *fe);

/* 按前端 id 查找：用户覆盖目录 → 安装目录。成功时 *out 是绝对路径 */
RhErr rh_frontend_find(const char *id, gchar **out_path);

typedef struct {
  const char *dest_dir;        /* 必填 */
  RhFrontend *frontend;        /* 必填 */
  GPtrArray  *platforms;       /* GPtrArray<gchar*>，NULL/空 = 全部 */
  GPtrArray  *ids;             /* GPtrArray<gchar*>，NULL/空 = 全部 */
  gboolean    media_all;       /* TRUE = 复制全部 7 类 */
  gboolean    media_kinds[7];  /* media_all 为 FALSE 时逐类开关 */
  gboolean    write_roms;      /* 默认 TRUE */
  gboolean    generate_m3u;    /* 多碟生成 m3u */
  gboolean    compress;        /* M3 未实现：置 TRUE 返回 UNSUPPORTED */
} RhExportOpts;

typedef struct {
  gint64 games;
  gint64 skipped;
  gint64 roms;
  gint64 media;
} RhExportResult;

RhErr rh_export_run(RhCtx *ctx, const RhExportOpts *opts,
                    RhExportResult *result);

/* 供 CLI 把 --media 等选项映射成 kind 下标；未知返回 -1 */
int rh_export_kind_index(const char *kind);
