/*
 * rh-records.h — 领域记录结构体（ARCHITECTURE §3.2 / §4）
 *
 * 纯数据：不含 SQL、不打印、不碰文件系统。谁拥有内存，在注释里写明。
 * 约定：数值字段用哨兵表示"未知"——year/players 用 0，rating 用 -1，
 * size 用 -1；入库时转 NULL，读出时再还原成哨兵，前端不需要区分 NULL。
 */
#pragma once

#include <glib.h>

typedef struct {
  gchar   *id;                 /* 主键；rh_game_gen_id() 从 dir_path 派生 */
  gchar   *platform;           /* 平台 ID（platforms.conf 的键） */
  gchar   *name;               /* 显示名 */
  gchar   *game_main_name;     /* 多碟合并后的主名；单碟时可为 NULL */
  gchar   *dir_path;           /* 游戏目录（库内相对或绝对），UNIQUE */
  gchar   *developer;
  gchar   *publisher;
  int      year;               /* 0 = 未知 */
  gchar   *genre;
  int      players;            /* 0 = 未知 */
  double   rating;             /* -1 = 未知；0.0–1.0 */
  gchar   *region;
  gchar   *description;
  gboolean is_multi_disc;
  gboolean is_arcade;
  gchar   *rom_format;         /* raw / zip / 7z / chd / cso / wbfs … */
  gchar   *rom_status;         /* good / baddump / unknown */
  gchar   *dat_source;         /* DAT 名称；NULL = 未匹配 */
  gchar   *patch_type;         /* "" / generic / platform / mixed */
  gchar   *created_at;         /* ISO 8601，入库时自动填 */
  gchar   *updated_at;
} RhGame;

RhGame *rh_game_new(void);
RhGame *rh_game_copy(const RhGame *src);
void    rh_game_free(RhGame *g);

/* 从 dir_path 派生稳定主键：<platform>-<sha1 前 12 位>
 * 同一目录重复导入得到同一个 id，因此 upsert 天然幂等。调用方 g_free。 */
gchar *rh_game_gen_id(const char *platform, const char *dir_path);

/* --------------------------------------------------------------- 子表记录 */

typedef struct {
  gchar  *filename;            /* 相对游戏目录的文件名 */
  int     disc_no;             /* 0 = 单碟；>=1 = 碟号 */
  gint64  size;                /* -1 = 未统计 */
  gchar  *crc32;
  gchar  *md5;
  gchar  *sha1;
  int     sort;
} RhRomFile;

RhRomFile *rh_rom_file_new(void);
void       rh_rom_file_free(RhRomFile *r);

typedef struct {
  gchar  *kind;                /* covers/screenshots/… 见 platform.c */
  gchar  *filename;
  gint64  size;                /* -1 = 未统计 */
  int     sort;
} RhMediaFile;

RhMediaFile *rh_media_file_new(void);
void         rh_media_file_free(RhMediaFile *m);

/* DAT 命中结果（store_dat_find_by_hash 填充） */
typedef struct {
  gint64 dat_id;
  gchar *platform;
  gchar *dat_name;
  gchar *game_name;
  gchar *rom_name;
  gint64 size;
} RhDatMatch;

RhDatMatch *rh_dat_match_new(void);
void        rh_dat_match_free(RhDatMatch *m);

/* DAT 条目（dat.c 解析 Logiqx 后批量交给 store 落库） */
typedef struct {
  gchar  *game_name;
  gchar  *rom_name;
  gint64  size;      /* -1 = 未知 */
  gchar  *crc32;
  gchar  *md5;
  gchar  *sha1;
} RhDatEntry;

RhDatEntry *rh_dat_entry_new(void);
void        rh_dat_entry_free(RhDatEntry *e);

/* 作业条目（导入/导出队列，崩溃后可续） */
typedef struct {
  gint64 job_id;
  int    seq;
  gchar *src;
  gchar *action;      /* rom / media / patcher / skip */
  gchar *platform;
  gchar *target_id;
  gchar *state;       /* pending / running / done / failed / skipped */
  gchar *message;
} RhJobItem;

RhJobItem *rh_job_item_new(void);
void       rh_job_item_free(RhJobItem *it);

/* --------------------------------------------------------------- 筛选条件 */

/*
 * 13 维筛选，与 1.0.0 的 cmd_list 参数一一对应（ARCHITECTURE §4）。
 * 指针字段为 NULL = 不限；multi / has_patch 用 -1 = 不限。
 */
typedef struct {
  const char *platform;
  const char *genre;
  const char *developer;
  const char *publisher;
  const char *region;
  const char *patch_type;
  int         year_from;       /* 0 = 不限 */
  int         year_to;         /* 0 = 不限 */
  int         multi;           /* -1 不限 / 0 / 1 */
  int         has_patch;       /* -1 不限 / 0 / 1 */
  const char *status;
  const char *format;
  const char **tags;
  int         n_tags;
  const char *kw;              /* 走 FTS5；trigram 不可用时退化为 LIKE */
  const char *sort;            /* name / rating / year / discs */
  int         desc;
} RhFilter;

RhFilter *rh_filter_new(void);   /* 全部"不限" */
void      rh_filter_free(RhFilter *f);

/* --------------------------------------------------------------- 作业记录 */

typedef struct {
  gint64  id;
  gchar  *kind;                /* import / export / dat-import */
  gchar  *state;               /* queued/running/paused/done/failed/cancelled */
  int     total;
  int     done;
  gchar  *created_at;
  gchar  *finished_at;
  gchar  *error;
} RhJob;

RhJob *rh_job_new(void);
void   rh_job_free(RhJob *j);

/* ISO 8601 本地时间（"2026-11-11T14:03:00+08:00"）；调用方 g_free。 */
gchar *rh_now_iso(void);
