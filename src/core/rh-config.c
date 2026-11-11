#include "rh-config.h"

#include <string.h>

#include "rh-dirs.h"
#include "rh-fs.h"

struct RhConfig {
  GHashTable *values;   /* key -> RhConfigValue* */
};

/* ------------------------------------------------------------------ 内置默认 */

static const struct {
  const char *key;
  const char *value;
} builtin_defaults[] = {
  /* 沿用 1.0.0 的键名与默认值（ARCHITECTURE §10） */
  { "ROM_ROOT",                        ""               },
  { "dat_dir",                         ""               },
  { "archive_extensions",              ".zip;.7z;.rar"  },
  { "archive_compress_export_format",  "zip"            },
  { "duplicate_check_threshold",       "70"             },
  { "dat_match_hash",                  "crc32"          },
  { "dat_auto_match_multi_disc",       "true"           },
  { "auto_generate_m3u",               "true"           },
  { "export_hidden_discs",             "true"           },
  { "disc_label_prefix",               "Disc"           },
  { "auto_delete_source",              "false"          },
  /* 本次新增 */
  { "store_path",                      ""               },
  { "emulator_launch",                 ""               },
  { "lang",                            ""               },
};

static void config_value_free(gpointer p)
{
  RhConfigValue *v = p;
  if (!v)
    return;
  g_free(v->value);
  g_free(v->source_file);
  g_free(v->shadowed_value);
  g_free(v->shadowed_file);
  g_free(v);
}

RhConfig *rh_config_new(void)
{
  RhConfig *c = g_new0(RhConfig, 1);

  c->values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                    config_value_free);

  for (guint i = 0; i < G_N_ELEMENTS(builtin_defaults); i++) {
    RhConfigValue *v = g_new0(RhConfigValue, 1);
    v->value = g_strdup(builtin_defaults[i].value);
    v->layer = RH_LAYER_BUILTIN;
    g_hash_table_replace(c->values, g_strdup(builtin_defaults[i].key), v);
  }
  return c;
}

void rh_config_free(RhConfig *c)
{
  if (!c)
    return;
  g_hash_table_destroy(c->values);
  g_free(c);
}

static RhConfigValue *value_new(const char *value, RhLayer layer,
                                const char *file, int line)
{
  RhConfigValue *v = g_new0(RhConfigValue, 1);
  v->value = g_strdup(value ? value : "");
  v->layer = layer;
  v->source_file = file ? g_strdup(file) : NULL;
  v->line = line;
  return v;
}

void rh_config_set(RhConfig *c, const char *key, const char *value,
                   RhLayer layer)
{
  RhConfigValue *old = g_hash_table_lookup(c->values, key);
  RhConfigValue *nv = value_new(value, layer, NULL, 0);

  if (old) {
    nv->shadowed_value = g_strdup(old->value);
    nv->shadowed_file = old->source_file ? g_strdup(old->source_file) : NULL;
    nv->shadowed_line = old->line;
    nv->shadowed_layer = old->layer;
  }
  g_hash_table_replace(c->values, g_strdup(key), nv);
}

const RhConfigValue *rh_config_get_value(RhConfig *c, const char *key)
{
  if (!c || !key)
    return NULL;
  return g_hash_table_lookup(c->values, key);
}

const gchar *rh_config_get(RhConfig *c, const char *key)
{
  const RhConfigValue *v = rh_config_get_value(c, key);
  return v ? v->value : NULL;
}

gboolean rh_config_get_bool(RhConfig *c, const char *key)
{
  const gchar *s = rh_config_get(c, key);
  if (!s)
    return FALSE;
  return g_ascii_strcasecmp(s, "true") == 0 || g_ascii_strcasecmp(s, "yes") == 0 ||
         g_ascii_strcasecmp(s, "1") == 0 || g_ascii_strcasecmp(s, "on") == 0;
}

int rh_config_get_int(RhConfig *c, const char *key, int fallback)
{
  const gchar *s = rh_config_get(c, key);
  gint64 v;

  if (!s || !*s)
    return fallback;
  v = g_ascii_strtoll(s, NULL, 10);
  if (v == 0 && *s != '0')
    return fallback;
  return (int) v;
}

/* ------------------------------------------------------------------ 文件加载 */

/*
 * 自己解析而不是用 GKeyFile：GKeyFile 不暴露行号，而 §10.1 的
 * `config --explain` 必须能报出 "文件:行"。格式是 GKeyFile 的最小子集：
 *   [default] / key = value / # 与 ; 注释 / 空行
 * 其他段落的键一律忽略（1.0.0 也只读 [default]）。
 */
RhErr rh_config_load_file(RhConfig *c, RhLayer layer, const char *path)
{
  gchar **lines;
  gchar  *content = NULL;
  GError *gerr = NULL;
  gchar  *group = NULL;
  RhErr   ret = rh_ok();

  if (!g_file_get_contents(path, &content, NULL, &gerr)) {
    g_error_free(gerr);
    return rh_ok();   /* 文件不存在 = 该层不覆盖，不是错误 */
  }

  lines = g_strsplit(content, "\n", -1);
  for (int i = 0; lines[i]; i++) {
    gchar *line = g_strstrip(lines[i]);
    gchar *eq;

    if (*line == '\0' || *line == '#' || *line == ';')
      continue;

    if (*line == '[') {
      gchar *end = strchr(line, ']');
      if (!end) {
        ret = rh_fail(RH_ERR_PARSE, "%s:%d: malformed section header", path,
                      i + 1);
        goto out;
      }
      g_free(group);
      group = g_strndup(line + 1, (gsize) (end - line - 1));
      continue;
    }

    eq = strchr(line, '=');
    if (!eq) {
      ret = rh_fail(RH_ERR_PARSE, "%s:%d: expected key = value", path, i + 1);
      goto out;
    }

    if (group && g_ascii_strcasecmp(group, "default") != 0)
      continue;

    {
      gchar *key = g_strndup(line, (gsize) (eq - line));
      gchar *val = g_strdup(eq + 1);
      g_strstrip(key);
      g_strstrip(val);
      if (*key == '\0') {
        g_free(key);
        g_free(val);
        ret = rh_fail(RH_ERR_PARSE, "%s:%d: empty key", path, i + 1);
        goto out;
      }

      RhConfigValue *old = g_hash_table_lookup(c->values, key);
      RhConfigValue *nv = value_new(val, layer, path, i + 1);
      if (old) {
        nv->shadowed_value = g_strdup(old->value);
        nv->shadowed_file = old->source_file ? g_strdup(old->source_file) : NULL;
        nv->shadowed_line = old->line;
        nv->shadowed_layer = old->layer;
      }
      g_hash_table_replace(c->values, key, nv);
      g_free(val);
    }
  }

out:
  g_free(group);
  g_strfreev(lines);
  g_free(content);
  return ret;
}

const char *rh_config_factory_path(void)
{
  return RH_DATADIR "/rom-hamster.conf";
}

const char *rh_config_system_path(void)
{
  return RH_SYSCONFDIR "/rom-hamster.conf";
}

gchar *rh_config_user_path(void)
{
  return g_build_filename(g_get_user_config_dir(), "rom-hamster",
                          "rom-hamster.conf", NULL);
}

void rh_config_load_standard(RhConfig *c)
{
  gchar *user_path = rh_config_user_path();

  rh_config_load_file(c, RH_LAYER_FACTORY, rh_config_factory_path());
  rh_config_load_file(c, RH_LAYER_SYSTEM, rh_config_system_path());
  rh_config_load_file(c, RH_LAYER_USER, user_path);

  g_free(user_path);
}

/* ------------------------------------------------------------------ 解释输出 */

const char *rh_layer_name(RhLayer layer)
{
  switch (layer) {
  case RH_LAYER_BUILTIN:  return "built-in default";
  case RH_LAYER_FACTORY:  return "factory (/usr/share)";
  case RH_LAYER_SYSTEM:   return "system (/etc)";
  case RH_LAYER_USER:     return "user (~/.config)";
  case RH_LAYER_CLI:      return "command line";
  default:                return "unknown layer";
  }
}

gchar *rh_config_explain(RhConfig *c, const char *key)
{
  const RhConfigValue *v = rh_config_get_value(c, key);
  GString *s;

  if (!v)
    return g_strdup_printf("%s is not a known key\n", key);

  s = g_string_new(NULL);
  g_string_append_printf(s, "%s = %s\n", key, v->value);

  if (v->source_file)
    g_string_append_printf(s, "  from %s:%d (%s)\n", v->source_file, v->line,
                           rh_layer_name(v->layer));
  else
    g_string_append_printf(s, "  from %s\n", rh_layer_name(v->layer));

  if (v->shadowed_value) {
    if (v->shadowed_file)
      g_string_append_printf(s, "  overrides %s from %s:%d (%s)\n",
                             v->shadowed_value, v->shadowed_file,
                             v->shadowed_line,
                             rh_layer_name(v->shadowed_layer));
    else
      g_string_append_printf(s, "  overrides %s from %s\n",
                             v->shadowed_value,
                             rh_layer_name(v->shadowed_layer));
  }
  return g_string_free(s, FALSE);
}

gchar **rh_config_keys(RhConfig *c, guint *n_out)
{
  GList  *keys = g_hash_table_get_keys(c->values);
  GList  *l;
  guint   n;
  guint   i = 0;
  gchar **out;

  keys = g_list_sort(keys, (GCompareFunc) g_strcmp0);
  n = g_list_length(keys);
  out = g_new0(gchar *, n + 1);

  for (l = keys; l; l = l->next)
    out[i++] = g_strdup(l->data);

  g_list_free(keys);
  if (n_out)
    *n_out = n;
  return out;
}

RhErr rh_config_save_user_key(const char *key, const char *value)
{
  RhErr e = rh_ok();
  gchar *path = rh_config_user_path();
  gchar *old = NULL;
  GString *out = g_string_new(NULL);
  gchar **lines = NULL;
  gboolean replaced = FALSE;

  if (!key || !*key || !value)
    return rh_fail(RH_ERR_CONFIG, "config: empty key");

  if (!g_file_get_contents(path, &old, NULL, NULL))
    old = g_strdup("");

  lines = g_strsplit(old, "\n", -1);
  for (int i = 0; lines[i]; i++) {
    gchar *line = lines[i];
    gchar *orig = g_strdup(line);
    gchar *trim = g_strstrip(line);
    if (!replaced && *trim && *trim != '#' && *trim != ';') {
      gchar *eq = strchr(trim, '=');
      if (eq) {
        gchar *k = g_strndup(trim, (gsize)(eq - trim));
        gchar *kt = g_strstrip(k);
        if (g_strcmp0(kt, key) == 0) {
          g_string_append_printf(out, "%s=%s\n", key, value);
          replaced = TRUE;
        } else {
          g_string_append_printf(out, "%s\n", orig);
        }
        g_free(k);
        g_free(orig);
        continue;
      }
    }
    g_string_append_printf(out, "%s\n", orig);
    g_free(orig);
  }
  g_strfreev(lines);

  if (!replaced)
    g_string_append_printf(out, "%s=%s\n", key, value);
  if (out->len && out->str[out->len - 1] == '\n')
    g_string_truncate(out, out->len - 1);

  {
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
  }
  if (rh_fs_write_text_atomic(path, out->str, &e) != 0)
    e = rh_fail(RH_ERR_IO, "config: cannot write %s", path);

  g_string_free(out, TRUE);
  g_free(old);
  g_free(path);
  return e;
}
