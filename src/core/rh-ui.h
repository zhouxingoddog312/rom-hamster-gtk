/*
 * rh-ui.h — 前端回调表（ARCHITECTURE §6.1）
 *
 * 核心库不认终端也不认 GTK：所有交互都经这张 vtable。没有注入 ui 时，
 * 核心层只会静默跳过（绝不自己打印、绝不读 stdin）。
 */
#pragma once

#include <glib.h>

typedef enum {
  RH_LOG_DEBUG,
  RH_LOG_INFO,
  RH_LOG_WARN,
  RH_LOG_ERROR,
} RhLogLevel;

/* 与 media.kind 的 CHECK 约束一致 */
typedef enum {
  RH_MEDIA_COVERS = 0,
  RH_MEDIA_SCREENSHOTS,
  RH_MEDIA_VIDEOS,
  RH_MEDIA_MARQUEES,
  RH_MEDIA_MIXIMAGES,
  RH_MEDIA_INTRO,
  RH_MEDIA_PATCHER,
  RH_MEDIA_KIND_COUNT
} RhMediaKind;

typedef struct RhUi {
  void *userdata;

  void (*log)(void *ud, RhLogLevel lv, const char *msg);

  /* pct < 0 表示不确定进度（脉冲模式）。工作线程只允许调这一个回调 */
  void (*progress)(void *ud, double pct, const char *label);

  /* 返回 1 = 是，0 = 否 */
  int (*confirm)(void *ud, const char *title, const char *msg, int default_yes);

  /* 返回选中索引，-1 = 取消；remember_key 非空时前端可提供"应用到全部" */
  int (*choose)(void *ud, const char *title, const char **items, int n,
                const char *remember_key);

  /* 返回新分配的字符串（调用方 g_free），NULL = 取消 */
  char *(*enter)(void *ud, const char *title, const char *label, const char *def);
  char *(*password)(void *ud, const char *title, const char *label);

  /* 返回 GPtrArray<gchar*>（调用方 g_ptr_array_unref），失败/取消返回 NULL */
  GPtrArray *(*pick_files)(void *ud, const char *title, int multiple);
  char      *(*pick_dir)(void *ud, const char *title);

  /* 只读文本展示（详情、报告） */
  void (*text_view)(void *ud, const char *title, const char *text);

  /* 返回 RhMediaKind，-1 = 取消；remember_key 语义同 choose */
  int (*ask_media_kind)(void *ud, const char *path, const char *remember_key);
} RhUi;
