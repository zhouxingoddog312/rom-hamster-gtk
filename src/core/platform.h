/*
 * platform.h — 平台规则（ARCHITECTURE §5 / §10.2）
 *
 * platforms.conf 五段一次性解析成预编译结构：
 *   [platform_extensions]  平台 → 扩展名/补丁/二义/别名
 *   [arcade_rom_suffixes]  街机 ZIP 内 ROM 后缀
 *   [media_type_dirs]      媒体类型 → 游戏目录下的子目录名
 *   [media_suffix_rules]   媒体类型 → 允许的扩展名
 *   [archive_extensions]   归档后缀（压缩包分流用）
 *
 * 边界（§5.1）：只解析规则、只做"扩展名/文件名 → 归属"的判定；
 * 不读文件内容、不碰数据库、不搬文件、不打印。
 */
#pragma once

#include <glib.h>

#include "rh-err.h"

/* 数据库 media.kind 的取值，与 store v1 的 CHECK 约束一一对应 */
#define RH_MEDIA_KIND_COVERS     "covers"
#define RH_MEDIA_KIND_SCREENSHOTS "screenshots"
#define RH_MEDIA_KIND_VIDEOS     "videos"
#define RH_MEDIA_KIND_MARQUEES   "marquees"
#define RH_MEDIA_KIND_MIXIMAGES  "miximages"
#define RH_MEDIA_KIND_INTRO      "intro"
#define RH_MEDIA_KIND_PATCHER    "patcher"

typedef struct {
  gchar    *id;
  GPtrArray *extensions;        /* ".nes"… 小写、带点 */
  gboolean  compress_supported;
  gchar    *recommended_format; /* zip / 7z / chd / cso / wbfs / raw */
  gboolean  arcade;
  GPtrArray *patch_formats;     /* ".ips"… */
  GPtrArray *ambiguous_formats; /* 既可能本体也可能补丁 */
  GPtrArray *aliases;           /* 目录名/前端别名 */
  gchar    *source_file;        /* 定义来自哪个文件 */
  int       line;
} RhPlatform;

typedef struct RhPlatformRules RhPlatformRules;

RhPlatformRules *platform_rules_new(void);
void             platform_rules_free(RhPlatformRules *r);

/* 单文件合并进规则集；文件不存在不是错误（该层不覆盖）。
 * 语法错误返回 RH_ERR_PARSE，消息里带 "文件:行"。 */
RhErr platform_rules_load(RhPlatformRules *r, const char *path);

/* 按 §10.2 的四层顺序装载 factory → system → user（后层覆盖前层） */
RhErr platform_rules_load_standard(RhPlatformRules *r);

guint        platform_count(RhPlatformRules *r);
RhPlatform  *platform_get(RhPlatformRules *r, const char *id);

/* 借用：GPtrArray<RhPlatform*>，按 id 升序；调用方不释放容器 */
GPtrArray   *platform_all(RhPlatformRules *r);

/* 扩展名 → 平台集合（借用）。0 个/1 个/多个由调用方决定策略：
 * 多个时必须走 ui->choose（ARCHITECTURE §5 平台判定） */
GPtrArray   *platform_by_extension(RhPlatformRules *r, const char *ext);

/* 目录名反查平台（先按 id，再按 aliases）；不命中返回 NULL */
RhPlatform  *platform_resolve_dir(RhPlatformRules *r, const char *dirname);

/* 文件名所属平台候选（借用）；等价于 platform_by_extension(ext_of(name)) */
GPtrArray   *platform_candidates_for_file(RhPlatformRules *r,
                                          const char *filename);

gboolean platform_is_archive(RhPlatformRules *r, const char *ext);
gboolean platform_is_arcade_rom_suffix(RhPlatformRules *r, const char *ext);

/* 媒体类型识别：返回 7 个 kind 之一，ROM/未知返回 NULL。
 * 优先级按 [media_type_dirs] 的书写顺序（无 [media_type_dirs] 时为内置顺序） */
const gchar *platform_media_kind(RhPlatformRules *r, const char *filename);

/* 媒体类型 → 子目录名；未知 kind 回退到 kind 本身 */
const gchar *platform_media_dir(RhPlatformRules *r, const char *kind);

/* 小写、带点的扩展名；无扩展名返回 NULL（调用方 g_free） */
gchar *platform_ext_of(const char *filename);
