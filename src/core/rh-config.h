/*
 * rh-config.h — 分层配置（ARCHITECTURE §10.1）
 *
 * 五层，自低到高：
 *   builtin → /usr/share/rom-hamster → /etc/rom-hamster → ~/.config/rom-hamster → 命令行
 * 每个键保留 {value, layer, source_file, line}，后者覆盖前者但记录被覆盖值，
 * 供 `rom-hamster config --explain` 一眼回答"为什么我的设置没生效"。
 */
#pragma once

#include <glib.h>

#include "rh-err.h"

typedef enum {
  RH_LAYER_BUILTIN = 0,   /* 编译进二进制的最小集 */
  RH_LAYER_FACTORY,       /* /usr/share/rom-hamster 出厂规则 */
  RH_LAYER_SYSTEM,        /* /etc/rom-hamster 系统覆盖（conffile） */
  RH_LAYER_USER,          /* ~/.config/rom-hamster 用户层 */
  RH_LAYER_CLI,           /* 命令行覆盖 */
  RH_LAYER_COUNT
} RhLayer;

typedef struct {
  gchar  *value;
  RhLayer layer;
  gchar  *source_file;     /* builtin 层为 NULL */
  int     line;

  /* 被本层覆盖掉的上一个值 */
  gchar  *shadowed_value;
  gchar  *shadowed_file;
  int     shadowed_line;
  RhLayer shadowed_layer;
} RhConfigValue;

typedef struct RhConfig RhConfig;

RhConfig  *rh_config_new(void);
void       rh_config_free(RhConfig *c);

/* 读一个 GKeyFile 风格的文件（只认 [default] 段，允许无段头）。缺文件不是错误。 */
RhErr      rh_config_load_file(RhConfig *c, RhLayer layer, const char *path);

/* 按标准路径装 factory / system / user 三层（文件不存在则跳过） */
void       rh_config_load_standard(RhConfig *c);

/* 程序化覆盖（命令行、GUI 设置页） */
void       rh_config_set(RhConfig *c, const char *key, const char *value,
                         RhLayer layer);

const gchar        *rh_config_get(RhConfig *c, const char *key);
const RhConfigValue *rh_config_get_value(RhConfig *c, const char *key);
gboolean            rh_config_get_bool(RhConfig *c, const char *key);
int                 rh_config_get_int(RhConfig *c, const char *key,
                                      int fallback);

/* 供 config --explain：多行文本，调用方 g_free */
gchar      *rh_config_explain(RhConfig *c, const char *key);

/* 已定义的键（含 builtin），按字母序 */
gchar     **rh_config_keys(RhConfig *c, guint *n_out);

const char *rh_layer_name(RhLayer layer);

/* 标准路径（供 --explain 与错误消息使用） */
const char *rh_config_factory_path(void);
const char *rh_config_system_path(void);
gchar      *rh_config_user_path(void);
