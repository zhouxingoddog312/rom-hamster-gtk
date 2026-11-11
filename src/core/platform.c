#include "platform.h"

#include <stdarg.h>
#include <string.h>

#include "rh-dirs.h"

struct RhPlatformRules {
  GHashTable *by_id;          /* id → RhPlatform*（不持有所有权） */
  GPtrArray  *platforms;      /* 持有所有权；按 id 排序 */
  GHashTable *by_ext;         /* ext → GPtrArray<RhPlatform*>（借用） */
  GHashTable *by_alias;       /* alias/id → RhPlatform*（借用） */

  GPtrArray  *arcade_suffixes;    /* gchar* ".rom" */
  GPtrArray  *archive_exts;       /* gchar* ".zip" */

  GHashTable *media_dirs;         /* kind → gchar* 子目录名 */
  GPtrArray  *media_kind_order;   /* gchar* kind，顺序决定识别优先级 */
  GHashTable *media_suffix;       /* kind → GPtrArray<gchar*> ext */
};

/* [media_type_dirs] 缺失时的兜底顺序：文件里没写也能识别 */
static const char *const default_media_order[] = {
  "covers", "screenshots", "videos", "marquees", "miximages", "intro", "patcher",
};

/* ---------------------------------------------------------------- 扩展名 */

gchar *platform_ext_of(const char *filename)
{
  const char *dot;

  if (!filename || !*filename)
    return NULL;
  dot = strrchr(filename, '.');
  if (!dot)
    return NULL;
  return g_ascii_strdown(dot, -1);
}

/* 规则文件里的扩展名写法容错：大小写、缺前导点 */
static gchar *ext_norm(const char *raw)
{
  gchar *s, *out;

  if (!raw)
    return NULL;
  s = g_strdup(raw);
  g_strstrip(s);
  {
    gchar *low = g_ascii_strdown(s, -1);   /* 注意：返回新串，不原地改 */
    g_free(s);
    s = low;
  }
  if (!*s) {
    g_free(s);
    return NULL;
  }
  if (*s == '.')
    return s;
  out = g_strconcat(".", s, NULL);
  g_free(s);
  return out;
}

static void append_ext(GPtrArray *arr, const char *raw)
{
  gchar *e = ext_norm(raw);

  if (!e)
    return;
  for (guint i = 0; i < arr->len; i++) {
    if (g_strcmp0(g_ptr_array_index(arr, i), e) == 0) {
      g_free(e);
      return;
    }
  }
  g_ptr_array_add(arr, e);
}

/* ---------------------------------------------------------------- 生命周期 */

static void platform_free(gpointer p)
{
  RhPlatform *pl = p;

  if (!pl)
    return;
  g_free(pl->id);
  g_ptr_array_unref(pl->extensions);
  g_free(pl->recommended_format);
  g_ptr_array_unref(pl->patch_formats);
  g_ptr_array_unref(pl->ambiguous_formats);
  g_ptr_array_unref(pl->aliases);
  g_free(pl->source_file);
  g_free(pl);
}

static RhPlatform *platform_new(void)
{
  RhPlatform *p = g_new0(RhPlatform, 1);

  p->extensions        = g_ptr_array_new_with_free_func(g_free);
  p->patch_formats     = g_ptr_array_new_with_free_func(g_free);
  p->ambiguous_formats = g_ptr_array_new_with_free_func(g_free);
  p->aliases           = g_ptr_array_new_with_free_func(g_free);
  return p;
}

RhPlatformRules *platform_rules_new(void)
{
  RhPlatformRules *r = g_new0(RhPlatformRules, 1);

  r->by_id     = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  r->platforms = g_ptr_array_new_with_free_func(platform_free);
  r->by_ext    = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       (GDestroyNotify) g_ptr_array_unref);
  r->by_alias  = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  r->arcade_suffixes = g_ptr_array_new_with_free_func(g_free);
  r->archive_exts    = g_ptr_array_new_with_free_func(g_free);

  r->media_dirs       = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                              g_free);
  r->media_kind_order = g_ptr_array_new_with_free_func(g_free);
  r->media_suffix     = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                              (GDestroyNotify) g_ptr_array_unref);
  return r;
}

void platform_rules_free(RhPlatformRules *r)
{
  if (!r)
    return;
  g_hash_table_destroy(r->by_ext);
  g_hash_table_destroy(r->by_alias);
  g_hash_table_destroy(r->by_id);
  g_ptr_array_unref(r->platforms);
  g_ptr_array_unref(r->arcade_suffixes);
  g_ptr_array_unref(r->archive_exts);
  g_hash_table_destroy(r->media_dirs);
  g_ptr_array_unref(r->media_kind_order);
  g_hash_table_destroy(r->media_suffix);
  g_free(r);
}

/* ---------------------------------------------------------------- 索引 */

static gint cmp_platform_id(gconstpointer a, gconstpointer b)
{
  const RhPlatform *pa = *(const RhPlatform *const *) a;
  const RhPlatform *pb = *(const RhPlatform *const *) b;

  return g_strcmp0(pa->id, pb->id);
}

/* 平台被覆盖后重建 ext / alias 两张索引；索引里的指针都归 platforms 所有 */
static void reindex(RhPlatformRules *r)
{
  g_hash_table_remove_all(r->by_ext);
  g_hash_table_remove_all(r->by_alias);
  g_ptr_array_sort(r->platforms, cmp_platform_id);

  for (guint i = 0; i < r->platforms->len; i++) {
    RhPlatform *p = g_ptr_array_index(r->platforms, i);

    g_hash_table_replace(r->by_alias, g_strdup(p->id), p);

    for (guint j = 0; j < p->extensions->len; j++) {
      const gchar *ext = g_ptr_array_index(p->extensions, j);
      GPtrArray   *arr = g_hash_table_lookup(r->by_ext, ext);

      if (!arr) {
        arr = g_ptr_array_new();        /* 元素借用，不设 free_func */
        g_hash_table_replace(r->by_ext, g_strdup(ext), arr);
      }
      g_ptr_array_add(arr, p);
    }

    for (guint j = 0; j < p->aliases->len; j++) {
      const gchar *al = g_ptr_array_index(p->aliases, j);
      g_hash_table_replace(r->by_alias, g_strdup(al), p);
    }
  }
}

/* ---------------------------------------------------------------- 段解析 */

static RhErr parse_error(const char *path, int line, const char *fmt, ...)
  G_GNUC_PRINTF(3, 4);

static RhErr parse_error(const char *path, int line, const char *fmt, ...)
{
  va_list ap;
  char    buf[RH_ERR_MSG_MAX - 96];

  va_start(ap, fmt);
  g_vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return rh_fail(RH_ERR_PARSE, "%s:%d: %s", path, line, buf);
}

static gboolean parse_bool(const char *v, gboolean *out)
{
  if (g_ascii_strcasecmp(v, "true") == 0 || g_ascii_strcasecmp(v, "1") == 0 ||
      g_ascii_strcasecmp(v, "yes") == 0 || g_ascii_strcasecmp(v, "on") == 0) {
    *out = TRUE;
    return TRUE;
  }
  if (g_ascii_strcasecmp(v, "false") == 0 || g_ascii_strcasecmp(v, "0") == 0 ||
      g_ascii_strcasecmp(v, "no") == 0 || g_ascii_strcasecmp(v, "off") == 0) {
    *out = FALSE;
    return TRUE;
  }
  return FALSE;
}

typedef enum {
  FIELD_NONE = 0,
  FIELD_COMPRESS,
  FIELD_FORMAT,
  FIELD_ARCADE,
  FIELD_PATCH,
  FIELD_AMBIGUOUS,
  FIELD_ALIASES,
} PlatformField;

static PlatformField field_of_token(const char *tok, const gchar **value)
{
  static const struct {
    const char  *name;
    PlatformField field;
  } names[] = {
    { "compress_supported", FIELD_COMPRESS },
    { "recommended_format", FIELD_FORMAT },
    { "arcade",             FIELD_ARCADE },
    { "patch_formats",      FIELD_PATCH },
    { "ambiguous_formats",  FIELD_AMBIGUOUS },
    { "aliases",            FIELD_ALIASES },
  };

  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    gsize n = strlen(names[i].name);

    if (g_str_has_prefix(tok, names[i].name) && tok[n] == '=') {
      *value = tok + n + 1;
      return names[i].field;
    }
  }
  return FIELD_NONE;
}

static void add_aliases(RhPlatform *p, const gchar *value)
{
  gchar **parts = g_strsplit(value, ",", -1);

  for (int i = 0; parts[i]; i++) {
    gchar *a = g_strdup(parts[i]);
    gchar *low;

    g_strstrip(a);
    low = g_ascii_strdown(a, -1);
    if (*low)
      g_ptr_array_add(p->aliases, low);
    else
      g_free(low);
    g_free(a);
  }
  g_strfreev(parts);
}

static RhErr parse_platform_line(RhPlatformRules *r, const char *path, int line,
                                 const char *text)
{
  gchar **tok = g_strsplit(text, ";", -1);
  const char *colon = strchr(tok[0], ':');
  RhPlatform *p = NULL;
  PlatformField cur = FIELD_NONE;
  RhErr e = rh_ok();
  gchar *id = NULL;

  if (!colon) {
    e = parse_error(path, line, "expected 'platform:ext;...'");
    goto out;
  }

  id = g_strndup(tok[0], (gsize) (colon - tok[0]));
  g_strstrip(id);
  {
    gchar *low = g_ascii_strdown(id, -1);
    g_free(id);
    id = low;
  }
  if (!*id) {
    e = parse_error(path, line, "empty platform id");
    goto out;
  }

  p = platform_new();
  p->id = g_strdup(id);
  p->source_file = g_strdup(path);
  p->line = line;
  p->recommended_format = g_strdup("raw");

  append_ext(p->extensions, colon + 1);

  for (int i = 1; tok[i]; i++) {
    const gchar *val = NULL;
    PlatformField f = field_of_token(tok[i], &val);

    if (f != FIELD_NONE) {
      cur = f;
      switch (f) {
      case FIELD_COMPRESS:
        if (!parse_bool(val, &p->compress_supported)) {
          e = parse_error(path, line, "compress_supported needs true/false");
          goto out;
        }
        break;
      case FIELD_FORMAT:
        g_free(p->recommended_format);
        p->recommended_format = g_strdup(*val ? val : "raw");
        break;
      case FIELD_ARCADE:
        if (!parse_bool(val, &p->arcade)) {
          e = parse_error(path, line, "arcade needs true/false");
          goto out;
        }
        break;
      case FIELD_PATCH:
        append_ext(p->patch_formats, val);
        break;
      case FIELD_AMBIGUOUS:
        append_ext(p->ambiguous_formats, val);
        break;
      case FIELD_ALIASES:
        add_aliases(p, val);
        break;
      case FIELD_NONE:
      default:
        break;
      }
      continue;
    }

    /* 不是 key=value：要么是扩展名，要么是上一个列表字段的续写 */
    if (strchr(tok[i], '=') != NULL) {
      e = parse_error(path, line, "unknown field '%s'", tok[i]);
      goto out;
    }

    switch (cur) {
    case FIELD_NONE:
      append_ext(p->extensions, tok[i]);
      break;
    case FIELD_PATCH:
      append_ext(p->patch_formats, tok[i]);
      break;
    case FIELD_AMBIGUOUS:
      append_ext(p->ambiguous_formats, tok[i]);
      break;
    case FIELD_ALIASES:
      add_aliases(p, tok[i]);
      break;
    default:
      e = parse_error(path, line,
                      "value '%s' follows a scalar field; "
                      "expected 'key=value'", tok[i]);
      goto out;
    }
  }

  /* 同 id 覆盖：先前定义整条丢弃（管理员/用户层覆盖出厂层） */
  {
    RhPlatform *old = g_hash_table_lookup(r->by_id, id);

    if (old) {
      g_ptr_array_remove(r->platforms, old);
      g_hash_table_remove(r->by_id, id);
    }
  }
  g_hash_table_replace(r->by_id, g_strdup(id), p);
  g_ptr_array_add(r->platforms, p);
  p = NULL;

out:
  if (p)
    platform_free(p);
  g_free(id);
  g_strfreev(tok);
  return e;
}

static RhErr parse_list_line(GPtrArray *target, const char *path, int line,
                             const char *text)
{
  gchar **tok = g_strsplit(text, ";", -1);

  for (int i = 0; tok[i]; i++) {
    gchar *t = g_strdup(tok[i]);
    g_strstrip(t);
    if (*t)
      append_ext(target, t);
    g_free(t);
  }
  g_strfreev(tok);
  (void) path;
  (void) line;
  return rh_ok();
}

static RhErr parse_kv_line(RhPlatformRules *r, const char *group,
                           const char *path, int line, const char *text)
{
  const char *eq = strchr(text, '=');
  gchar *key, *val;

  if (!eq)
    return parse_error(path, line, "expected key = value");

  key = g_strndup(text, (gsize) (eq - text));
  val = g_strdup(eq + 1);
  g_strstrip(key);
  g_strstrip(val);
  {
    gchar *lower = g_ascii_strdown(key, -1);

    g_free(key);
    key = lower;
  }

  if (!*key) {
    g_free(key);
    g_free(val);
    return parse_error(path, line, "empty key");
  }

  if (g_strcmp0(group, "media_type_dirs") == 0) {
    g_hash_table_replace(r->media_dirs, g_strdup(key), g_strdup(val));
    /* 顺序决定识别优先级：重复声明时保留首次位置 */
    gboolean seen = FALSE;
    for (guint i = 0; i < r->media_kind_order->len; i++) {
      if (g_strcmp0(g_ptr_array_index(r->media_kind_order, i), key) == 0) {
        seen = TRUE;
        break;
      }
    }
    if (!seen)
      g_ptr_array_add(r->media_kind_order, g_strdup(key));
  } else if (g_strcmp0(group, "media_suffix_rules") == 0) {
    /* patcher_generic 归到 patcher：DB 的 kind 只有 7 类 */
    const gchar *kind = g_str_has_prefix(key, "patcher") ? "patcher" : key;
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);

    gchar **tok = g_strsplit(val, ";", -1);
    for (int i = 0; tok[i]; i++) {
      gchar *t = g_strdup(tok[i]);
      g_strstrip(t);
      if (*t)
        append_ext(arr, t);
      g_free(t);
    }
    g_strfreev(tok);
    g_hash_table_replace(r->media_suffix, g_strdup(kind), arr);
  } else {
    g_free(key);
    g_free(val);
    return parse_error(path, line, "section '%s' does not take key = value",
                       group);
  }

  g_free(key);
  g_free(val);
  return rh_ok();
}

/* ---------------------------------------------------------------- 装载 */

RhErr platform_rules_load(RhPlatformRules *r, const char *path)
{
  gchar  *content = NULL;
  GError *gerr = NULL;
  gchar **lines;
  gchar  *group = NULL;
  RhErr   e = rh_ok();

  if (!r || !path)
    return rh_fail(RH_ERR_CONFIG, "platform_rules_load: bad arguments");

  if (!g_file_get_contents(path, &content, NULL, &gerr)) {
    g_error_free(gerr);
    return rh_ok();      /* 文件不存在 = 该层不覆盖 */
  }

  lines = g_strsplit(content, "\n", -1);
  for (int i = 0; lines[i]; i++) {
    gchar *line = g_strstrip(lines[i]);

    if (*line == '\0' || *line == '#' || *line == ';')
      continue;

    if (*line == '[') {
      gchar *end = strchr(line, ']');

      if (!end) {
        e = parse_error(path, i + 1, "malformed section header");
        goto out;
      }
      g_free(group);
      group = g_strndup(line + 1, (gsize) (end - line - 1));
      g_strstrip(group);
      continue;
    }

    if (!group) {
      e = parse_error(path, i + 1, "line outside any section");
      goto out;
    }

    if (g_strcmp0(group, "platform_extensions") == 0)
      e = parse_platform_line(r, path, i + 1, line);
    else if (g_strcmp0(group, "arcade_rom_suffixes") == 0)
      e = parse_list_line(r->arcade_suffixes, path, i + 1, line);
    else if (g_strcmp0(group, "archive_extensions") == 0)
      e = parse_list_line(r->archive_exts, path, i + 1, line);
    else if (g_strcmp0(group, "media_type_dirs") == 0 ||
             g_strcmp0(group, "media_suffix_rules") == 0)
      e = parse_kv_line(r, group, path, i + 1, line);
    else
      e = parse_error(path, i + 1, "unknown section [%s]", group);

    if (!rh_err_ok(&e))
      goto out;
  }

  reindex(r);

out:
  g_free(group);
  g_strfreev(lines);
  g_free(content);
  return e;
}

RhErr platform_rules_load_standard(RhPlatformRules *r)
{
  gchar *user_path;
  RhErr  e;

  e = platform_rules_load(r, RH_DATADIR "/platforms.conf");
  if (!rh_err_ok(&e))
    return e;

  e = platform_rules_load(r, RH_SYSCONFDIR "/platforms.conf");
  if (!rh_err_ok(&e))
    return e;

  user_path = g_build_filename(g_get_user_config_dir(), "rom-hamster",
                               "platforms.conf", NULL);
  e = platform_rules_load(r, user_path);
  g_free(user_path);
  return e;
}

/* ---------------------------------------------------------------- 查询 */

guint platform_count(RhPlatformRules *r)
{
  return r ? r->platforms->len : 0;
}

RhPlatform *platform_get(RhPlatformRules *r, const char *id)
{
  if (!r || !id)
    return NULL;
  return g_hash_table_lookup(r->by_id, id);
}

GPtrArray *platform_all(RhPlatformRules *r)
{
  return r ? r->platforms : NULL;
}

GPtrArray *platform_by_extension(RhPlatformRules *r, const char *ext)
{
  if (!r || !ext)
    return NULL;
  return g_hash_table_lookup(r->by_ext, ext);
}

GPtrArray *platform_candidates_for_file(RhPlatformRules *r,
                                        const char *filename)
{
  gchar *ext = platform_ext_of(filename);
  GPtrArray *arr;

  if (!ext)
    return NULL;
  arr = platform_by_extension(r, ext);
  g_free(ext);
  return arr;
}

RhPlatform *platform_resolve_dir(RhPlatformRules *r, const char *dirname)
{
  gchar *key;
  RhPlatform *p;

  if (!r || !dirname || !*dirname)
    return NULL;

  key = g_strdup(dirname);
  g_strstrip(key);
  {
    gchar *low = g_ascii_strdown(key, -1);
    g_free(key);
    key = low;
  }

  p = g_hash_table_lookup(r->by_alias, key);

  /* 带前端前缀的目录名，如 "nintendo - nes"：取最后一段再试一次 */
  if (!p) {
    const gchar *sep = strstr(key, " - ");
    if (sep)
      p = g_hash_table_lookup(r->by_alias, sep + 3);
  }

  g_free(key);
  return p;
}

gboolean platform_is_archive(RhPlatformRules *r, const char *ext)
{
  gchar *n;

  if (!r || !ext)
    return FALSE;
  n = ext_norm(ext);
  if (!n)
    return FALSE;
  for (guint i = 0; i < r->archive_exts->len; i++) {
    if (g_strcmp0(g_ptr_array_index(r->archive_exts, i), n) == 0) {
      g_free(n);
      return TRUE;
    }
  }
  g_free(n);
  return FALSE;
}

gboolean platform_is_arcade_rom_suffix(RhPlatformRules *r, const char *ext)
{
  gchar *n;

  if (!r || !ext)
    return FALSE;
  n = ext_norm(ext);
  if (!n)
    return FALSE;
  for (guint i = 0; i < r->arcade_suffixes->len; i++) {
    if (g_strcmp0(g_ptr_array_index(r->arcade_suffixes, i), n) == 0) {
      g_free(n);
      return TRUE;
    }
  }
  g_free(n);
  return FALSE;
}

const gchar *platform_media_kind(RhPlatformRules *r, const char *filename)
{
  gchar *ext;
  const gchar *hit = NULL;

  if (!r || !filename)
    return NULL;
  ext = platform_ext_of(filename);
  if (!ext)
    return NULL;

  /* 按 [media_type_dirs] 的书写顺序，第一个命中的类型获胜：
   * .png 同时出现在 covers/screenshots/marquees/miximages，顺序决定归属 */
  if (r->media_kind_order->len > 0) {
    for (guint i = 0; i < r->media_kind_order->len && !hit; i++) {
      const gchar *kind = g_ptr_array_index(r->media_kind_order, i);
      GPtrArray *suffixes;

      if (g_strcmp0(kind, "rom") == 0)
        continue;    /* rom 不是媒体类型 */
      suffixes = g_hash_table_lookup(r->media_suffix, kind);
      if (!suffixes)
        continue;
      for (guint j = 0; j < suffixes->len; j++) {
        if (g_strcmp0(g_ptr_array_index(suffixes, j), ext) == 0) {
          hit = kind;
          break;
        }
      }
    }
  }

  if (!hit) {
    for (guint i = 0; i < G_N_ELEMENTS(default_media_order) && !hit; i++) {
      GPtrArray *suffixes = g_hash_table_lookup(r->media_suffix,
                                                default_media_order[i]);
      if (!suffixes)
        continue;
      for (guint j = 0; j < suffixes->len; j++) {
        if (g_strcmp0(g_ptr_array_index(suffixes, j), ext) == 0) {
          hit = default_media_order[i];
          break;
        }
      }
    }
  }

  g_free(ext);
  return hit;
}

const gchar *platform_media_dir(RhPlatformRules *r, const char *kind)
{
  const gchar *dir;

  if (!kind)
    return NULL;
  if (!r)
    return kind;
  dir = g_hash_table_lookup(r->media_dirs, kind);
  return dir ? dir : kind;
}
