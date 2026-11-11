/*
 * detail.h — 只读详情视图模型（ARCHITECTURE §5 / §5.1）
 *
 * 只做"把库里的记录和磁盘上的文件拼成一份可直接展示的结构"：
 *   - 元数据（RhGame）
 *   - ROM 文件清单
 *   - 8 类资源（rom / covers / screenshots / videos / marquees /
 *     miximages / intro / patcher），每项带 size 与 exists
 * 不格式化文字、不写库、不删文件。文本排版在 CLI，图文面板在 GUI。
 */
#pragma once

#include <glib.h>

#include "rh-ctx.h"
#include "rh-err.h"
#include "rh-records.h"

#define RH_DETAIL_GROUP_COUNT 8

typedef struct {
  gchar  *filename;      /* 相对游戏目录的文件名 */
  gchar  *path;          /* 绝对路径（拼好，方便前端直接打开） */
  gint64  size;          /* -1 = 取不到 */
  gboolean exists;
} RhDetailFile;

typedef struct {
  const gchar *kind;     /* covers … rom；静态字符串，不释放 */
  GPtrArray   *files;    /* GPtrArray<RhDetailFile*> */
} RhDetailGroup;

typedef struct {
  RhGame      *game;                   /* 拥有 */
  GPtrArray   *roms;                   /* GPtrArray<RhRomFile*>，拥有 */
  RhDetailGroup groups[RH_DETAIL_GROUP_COUNT];
  gchar       *dir;                    /* 游戏目录绝对路径，拥有 */
} RhGameDetail;

RhDetailFile *rh_detail_file_new(void);
void          rh_detail_file_free(RhDetailFile *f);

/* 读取一个游戏的详情；未找到 → RH_ERR_NOT_FOUND。*out 由函数分配。 */
RhErr rh_detail_load(RhCtx *ctx, const char *game_id, RhGameDetail **out);

void rh_detail_free(RhGameDetail *d);

/* 资源组名（按 §5 顺序）；用于 CLI/GUI 遍历，越界返回 NULL */
const gchar *rh_detail_group_kind(int index);
