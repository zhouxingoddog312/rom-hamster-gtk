/*
 * classify.h — 单文件分类（ARCHITECTURE §5，1.0.0 的 classify.sh）
 *
 * 优先级链（自上而下，第一个命中即定）：
 *   压缩包 → 通用补丁 → 平台二义后缀 → 平台专属补丁 → ROM 本体 → 媒体/说明
 * 重名扩展名（.jpg/.md 既是媒体也可能是 ROM）先按真实内容判身份（dual_role_kind）。
 */
#pragma once

#include <glib.h>

#include "platform.h"
#include "rh-err.h"

typedef enum {
  RH_CLASS_UNKNOWN = 0,
  RH_CLASS_ARCHIVE,
  RH_CLASS_PATCH_GENERIC,
  RH_CLASS_PATCH_PLATFORM,
  RH_CLASS_AMBIGUOUS,
  RH_CLASS_ROM,
  RH_CLASS_MEDIA,
} RhClassKind;

typedef struct {
  RhClassKind  kind;
  gchar       *ext;            /* 复合扩展名优先（.p8.png） */
  gchar       *platform;       /* 补丁/二义归属；ROM 仅当候选唯一时填 */
  GPtrArray   *candidates;     /* ROM 候选平台 id 列表（可能为空/多个） */
  gchar       *media_kind;     /* RH_CLASS_MEDIA 时的 media.kind */
} RhClass;

RhClass   *rh_class_new(void);
void       rh_class_free(RhClass *c);

/*
 * 分类一个文件。只看文件名/扩展名与（必要时）文件内容嗅探，
 * 不搬文件、不写库、不打印。返回 0 成功、1 无法识别（kind = UNKNOWN）。
 */
int rh_classify_file(RhPlatformRules *rules, const char *path,
                     RhClass *out, RhErr *err);

/* 复合扩展名：末层是媒体后缀且去掉它后仍是平台扩展（如 .p8.png）则合并 */
gchar *rh_rom_extension(RhPlatformRules *rules, const char *filename);

/* 内容判身份：images 按魔数、text 按可打印率；其余一律 rom */
const gchar *rh_dual_role_kind(const char *path, const char *ext);
