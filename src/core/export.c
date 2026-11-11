/*
 * export.c — 模板引擎与前端导出（ARCHITECTURE §5 / §7.2）
 *
 * 边界：遍历输出、模板渲染、媒体复制/压缩；**不改库**。
 * 模板只排版，路径计算、复制、m3u、冲突处理都在这里做。
 */
#include "export.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

#include "platform.h"
#include "rh-config.h"
#include "rh-dirs.h"
#include "rh-fs.h"
#include "rh-archive.h"
#include "store.h"

/* ============================================================== 模板节点 */

typedef enum {
  NODE_TEXT,
  NODE_VAR,
  NODE_IF,
  NODE_FOR,
} NodeType;

typedef struct RhTplNode {
  NodeType  type;
  gchar    *text;       /* TEXT：字面量；VAR：变量名 */
  gchar    *escape;     /* xml / json / text */
  int       line;
  GPtrArray *kids;      /* IF/FOR 主体 */
  GPtrArray *alt;       /* IF 的 else 分支 */
} RhTplNode;

typedef struct {
  gchar     *path;
  gchar     *section;
  GPtrArray *nodes;
} RhTemplate;

struct RhFrontend {
  gchar      *path;
  GHashTable *kv;          /* "section.key" → value */
  GHashTable *kv_line;     /* "section.key" → 行号 */
  RhTemplate *header;
  RhTemplate *game;
  RhTemplate *footer;
  GHashTable *media;       /* kind → RhTemplate* */
};

static const char *VARS[] = {
  "id", "name", "game_main_name", "platform", "platform_export_name",
  "description", "description_line", "developer", "publisher", "year",
  "genre", "players", "rating", "rating_norm", "releasedate_es", "region",
  "tags", "disc_no", "is_multi_disc", "is_arcade", "rom_status",
  "rom_format", "dat_source", "patch_type",
  "rom_file", "rom_rel", "rom_size", "cover_rel", "screenshot_rel",
  "marquee_rel", "video_rel", "miximage_rel", "intro_rel", "patcher_rel",
  "export_dir", "platform_dir", "platform_name", "media_dir", "game_name",
  "platform_extensions", "emulator_launch",
  /* 循环体内专用 */
  "media_rel", "media_file", "media_kind", "tag",
};

static const char *LOOP_VARS[] = {
  "covers", "screenshots", "videos", "marquees", "miximages", "intro",
  "patcher", "tags", "roms",
};

static gboolean in_list(const char *needle, const char **list, guint n)
{
  for (guint i = 0; i < n; i++) {
    if (g_strcmp0(needle, list[i]) == 0)
      return TRUE;
  }
  return FALSE;
}

static void node_free(RhTplNode *n)
{
  if (!n)
    return;
  g_free(n->text);
  g_free(n->escape);
  if (n->kids)
    g_ptr_array_unref(n->kids);
  if (n->alt)
    g_ptr_array_unref(n->alt);
  g_free(n);
}

static RhTplNode *node_new(NodeType t, int line)
{
  RhTplNode *n = g_new0(RhTplNode, 1);

  n->type = t;
  n->line = line;
  if (t == NODE_IF || t == NODE_FOR) {
    n->kids = g_ptr_array_new_with_free_func((GDestroyNotify) node_free);
    if (t == NODE_IF)
      n->alt = g_ptr_array_new_with_free_func((GDestroyNotify) node_free);
  }
  return n;
}

/* 词法：把一段模板拆成节点。失败返回 FALSE，err 里带 "文件:行"。 */
typedef struct {
  gchar *kind;      /* text / var / if / else / endif / for / endfor */
  gchar *var;
  gchar *escape;
  gchar *text;
  int    line;
} Token;

static void token_clear(Token *t)
{
  g_free(t->kind);
  g_free(t->var);
  g_free(t->escape);
  g_free(t->text);
  memset(t, 0, sizeof(*t));
}

static gboolean next_token(const gchar *s, gsize len, gsize *pos, int *line,
                           Token *t, const gchar *path, RhErr *err)
{
  gsize start = *pos;
  const gchar *pct = NULL;

  memset(t, 0, sizeof(*t));
  for (gsize i = start; i < len; i++) {
    if (s[i] == '%') {
      pct = s + i;
      break;
    }
  }
  if (!pct) {
    t->kind = g_strdup("text");
    t->text = g_strndup(s + start, len - start);
    t->line = *line;
    *pos = len;
    return TRUE;
  }

  if ((gsize)(pct - s) > start) {
    t->kind = g_strdup("text");
    t->text = g_strndup(s + start, (gsize)(pct - s) - start);
    t->line = *line;
    *pos = (gsize)(pct - s);
    return TRUE;
  }

  const gchar *end = memchr(pct + 1, '%', len - (gsize)(pct - s) - 1);
  if (!end) {
    rh_err_set(err, RH_ERR_PARSE, "%s:%d: unterminated '%%'", path, *line);
    return FALSE;
  }

  gchar *body = g_strndup(pct + 1, (gsize)(end - pct) - 1);
  g_strstrip(body);
  t->line = *line;

  if (g_strcmp0(body, "else") == 0) {
    t->kind = g_strdup("else");
  } else if (g_strcmp0(body, "endif") == 0) {
    t->kind = g_strdup("endif");
  } else if (g_strcmp0(body, "endfor") == 0) {
    t->kind = g_strdup("endfor");
  } else if (g_str_has_prefix(body, "if:")) {
    t->kind = g_strdup("if");
    t->var = g_strdup(body + 3);
  } else if (g_str_has_prefix(body, "for:")) {
    t->kind = g_strdup("for");
    t->var = g_strdup(body + 4);
  } else if (*body) {
    gchar *colon = strchr(body, ':');
    t->kind = g_strdup("var");
    if (colon) {
      *colon = '\0';
      t->var = g_strdup(body);
      t->escape = g_strdup(colon + 1);
    } else {
      t->var = g_strdup(body);
    }
  } else {
    rh_err_set(err, RH_ERR_PARSE, "%s:%d: empty template token", path, *line);
    g_free(body);
    return FALSE;
  }

  for (const gchar *q = pct; q <= end; q++) {
    if (*q == '\n')
      (*line)++;
  }
  *pos = (gsize)(end - s) + 1;
  g_free(body);
  return TRUE;
}

typedef struct {
  GPtrArray *list;
  RhTplNode *node;
  gboolean   in_else;
} Frame;

static gboolean parse_template(const gchar *path, const gchar *section,
                               const gchar *text, int base_line,
                               RhTemplate **out, RhErr *err)
{
  RhTemplate *tpl = g_new0(RhTemplate, 1);
  GPtrArray *stack = g_ptr_array_new();
  gsize pos = 0, len = strlen(text);
  int line = base_line;
  gboolean ok = TRUE;

  tpl->path = g_strdup(path);
  tpl->section = g_strdup(section);
  tpl->nodes = g_ptr_array_new_with_free_func((GDestroyNotify) node_free);

  Frame *root = g_new0(Frame, 1);
  root->list = tpl->nodes;
  g_ptr_array_add(stack, root);

  while (pos < len) {
    Token t;
    if (!next_token(text, len, &pos, &line, &t, path, err)) {
      ok = FALSE;
      break;
    }
    Frame *top = g_ptr_array_index(stack, stack->len - 1);

    if (g_strcmp0(t.kind, "text") == 0) {
      if (t.text && *t.text) {
        RhTplNode *n = node_new(NODE_TEXT, t.line);
        n->text = g_strdup(t.text);
        g_ptr_array_add(top->list, n);
      }
    } else if (g_strcmp0(t.kind, "var") == 0) {
      if (!t.var || !*t.var || !in_list(t.var, VARS, G_N_ELEMENTS(VARS))) {
        rh_err_set(err, RH_ERR_PARSE, "%s:%d: undefined template variable '%%%s%%'",
                   path, t.line, t.var ? t.var : "");
        ok = FALSE;
      } else if (t.escape &&
                 g_strcmp0(t.escape, "xml") != 0 &&
                 g_strcmp0(t.escape, "json") != 0 &&
                 g_strcmp0(t.escape, "text") != 0) {
        rh_err_set(err, RH_ERR_PARSE, "%s:%d: unknown escape ':%s'",
                   path, t.line, t.escape);
        ok = FALSE;
      } else {
        RhTplNode *n = node_new(NODE_VAR, t.line);
        n->text = g_strdup(t.var);
        n->escape = g_strdup(t.escape ? t.escape : "text");
        g_ptr_array_add(top->list, n);
      }
    } else if (g_strcmp0(t.kind, "if") == 0) {
      if (!t.var || !in_list(t.var, VARS, G_N_ELEMENTS(VARS))) {
        rh_err_set(err, RH_ERR_PARSE, "%s:%d: undefined condition '%%if:%s%%'",
                   path, t.line, t.var ? t.var : "");
        ok = FALSE;
      } else if (stack->len > 4) {
        rh_err_set(err, RH_ERR_PARSE, "%s:%d: condition nesting exceeds 3",
                   path, t.line);
        ok = FALSE;
      } else {
        RhTplNode *n = node_new(NODE_IF, t.line);
        n->text = g_strdup(t.var);
        g_ptr_array_add(top->list, n);
        Frame *f = g_new0(Frame, 1);
        f->list = n->kids;
        f->node = n;
        g_ptr_array_add(stack, f);
      }
    } else if (g_strcmp0(t.kind, "else") == 0) {
      if (!top->node || top->node->type != NODE_IF || top->in_else) {
        rh_err_set(err, RH_ERR_PARSE, "%s:%d: unexpected %%else%%", path, t.line);
        ok = FALSE;
      } else {
        top->in_else = TRUE;
        top->list = top->node->alt;
      }
    } else if (g_strcmp0(t.kind, "endif") == 0) {
      if (!top->node || top->node->type != NODE_IF) {
        rh_err_set(err, RH_ERR_PARSE, "%s:%d: unexpected %%endif%%", path, t.line);
        ok = FALSE;
      } else {
        g_free(top);
        g_ptr_array_remove_index(stack, stack->len - 1);
      }
    } else if (g_strcmp0(t.kind, "for") == 0) {
      if (!t.var || !in_list(t.var, LOOP_VARS, G_N_ELEMENTS(LOOP_VARS))) {
        rh_err_set(err, RH_ERR_PARSE, "%s:%d: unknown loop '%%for:%s%%'",
                   path, t.line, t.var ? t.var : "");
        ok = FALSE;
      } else if (stack->len > 4) {
        rh_err_set(err, RH_ERR_PARSE, "%s:%d: loop nesting exceeds 3",
                   path, t.line);
        ok = FALSE;
      } else {
        RhTplNode *n = node_new(NODE_FOR, t.line);
        n->text = g_strdup(t.var);
        g_ptr_array_add(top->list, n);
        Frame *f = g_new0(Frame, 1);
        f->list = n->kids;
        f->node = n;
        g_ptr_array_add(stack, f);
      }
    } else if (g_strcmp0(t.kind, "endfor") == 0) {
      if (!top->node || top->node->type != NODE_FOR) {
        rh_err_set(err, RH_ERR_PARSE, "%s:%d: unexpected %%endfor%%", path, t.line);
        ok = FALSE;
      } else {
        g_free(top);
        g_ptr_array_remove_index(stack, stack->len - 1);
      }
    }
    token_clear(&t);
    if (!ok)
      break;
  }

  if (ok && stack->len != 1) {
    Frame *top = g_ptr_array_index(stack, stack->len - 1);
    rh_err_set(err, RH_ERR_PARSE, "%s:%d: unclosed block", path,
               top->node ? top->node->line : base_line);
    ok = FALSE;
  }

  for (guint i = 0; i < stack->len; i++)
    g_free(g_ptr_array_index(stack, i));
  g_ptr_array_unref(stack);

  if (!ok) {
    g_free(tpl->path);
    g_free(tpl->section);
    g_ptr_array_unref(tpl->nodes);
    g_free(tpl);
    return FALSE;
  }
  *out = tpl;
  return TRUE;
}

static void template_free(RhTemplate *t)
{
  if (!t)
    return;
  g_free(t->path);
  g_free(t->section);
  if (t->nodes)
    g_ptr_array_unref(t->nodes);
  g_free(t);
}

/* ============================================================== 前端加载 */

static const char *kv_get(RhFrontend *fe, const char *sec, const char *key)
{
  gchar *k = g_strdup_printf("%s.%s", sec, key);
  const gchar *v = g_hash_table_lookup(fe->kv, k);

  g_free(k);
  return v;
}

static int kv_line(RhFrontend *fe, const char *sec, const char *key)
{
  gchar *k = g_strdup_printf("%s.%s", sec, key);
  gpointer v = g_hash_table_lookup(fe->kv_line, k);

  g_free(k);
  return v ? GPOINTER_TO_INT(v) : 0;
}

static gchar *trim_trailing_blank(const gchar *s)
{
  gchar *copy = g_strdup(s);
  gsize n = strlen(copy);

  while (n > 0 && g_ascii_isspace(copy[n - 1]))
    n--;
  copy[n] = '\0';
  gchar *r = g_strdup_printf("%s\n", copy);
  g_free(copy);
  return r;
}

RhErr rh_frontend_load(const char *path, RhFrontend **out)
{
  gchar *text = NULL;
  gsize len = 0;
  gchar **lines = NULL;
  RhFrontend *fe = NULL;
  GString *raw = NULL;
  int raw_line = 0;
  gchar *section = NULL;
  RhErr e = rh_ok();

  if (!path || !out)
    return rh_fail(RH_ERR_CONFIG, "frontend_load: bad arguments");
  *out = NULL;

  if (!g_file_get_contents(path, &text, &len, NULL))
    return rh_fail(RH_ERR_IO, "cannot read frontend spec: %s", path);

  fe = g_new0(RhFrontend, 1);
  fe->path = g_strdup(path);
  fe->kv = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  fe->kv_line = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  fe->media = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  lines = g_strsplit(text, "\n", -1);
  for (int i = 0; lines[i]; i++) {
    gchar *line = lines[i];
    gsize l = strlen(line);
    if (l && line[l - 1] == '\r')
      line[--l] = '\0';

    if (line[0] == '[') {
      gchar *close = strchr(line, ']');
      if (!close) {
        rh_err_set(&e, RH_ERR_PARSE, "%s:%d: malformed section header",
                   path, i + 1);
        goto fail;
      }
      if (raw && section) {
        gchar *body = trim_trailing_blank(raw->str);
        RhTemplate *tpl = NULL;
        if (!parse_template(path, section, body, raw_line, &tpl, &e)) {
          g_free(body);
          goto fail;
        }
        if (g_strcmp0(section, "export.header") == 0)
          fe->header = tpl;
        else if (g_strcmp0(section, "export.game") == 0)
          fe->game = tpl;
        else if (g_strcmp0(section, "export.footer") == 0)
          fe->footer = tpl;
        g_free(body);
        g_string_free(raw, TRUE);
        raw = NULL;
      }
      g_free(section);
      section = g_strndup(line + 1, (gsize)(close - line) - 1);
      g_strstrip(section);
      continue;
    }

    if (!section)
      continue;

    gboolean is_tpl = g_strcmp0(section, "export.header") == 0
                   || g_strcmp0(section, "export.game") == 0
                   || g_strcmp0(section, "export.footer") == 0;

    if (is_tpl) {
      gchar *tl = g_strdup(line);
      g_strstrip(tl);
      if (*tl == '#' || *tl == ';') {
        g_free(tl);
        continue;
      }
      g_free(tl);
      if (!raw) {
        raw = g_string_new(NULL);
        raw_line = i + 1;
      }
      g_string_append(raw, line);
      g_string_append_c(raw, '\n');
      continue;
    }

    gchar *trim = g_strdup(line);
    g_strstrip(trim);
    if (!*trim || trim[0] == '#' || trim[0] == ';') {
      g_free(trim);
      continue;
    }
    gchar *eq = strchr(trim, '=');
    if (!eq) {
      rh_err_set(&e, RH_ERR_PARSE, "%s:%d: expected key = value", path, i + 1);
      g_free(trim);
      goto fail;
    }
    *eq = '\0';
    gchar *key = g_strstrip(trim);
    gchar *val = g_strstrip(eq + 1);
    gchar *semi = strstr(val, " ;");
    if (semi)
      *semi = '\0';
    g_strstrip(val);

    gchar *full = g_strdup_printf("%s.%s", section, key);
    g_hash_table_replace(fe->kv, full, g_strdup(val));
    g_hash_table_replace(fe->kv_line, g_strdup(full),
                         GINT_TO_POINTER(i + 1));
    g_free(trim);
  }

  if (raw && section) {
    gchar *body = trim_trailing_blank(raw->str);
    RhTemplate *tpl = NULL;
    if (!parse_template(path, section, body, raw_line, &tpl, &e)) {
      g_free(body);
      goto fail;
    }
    if (g_strcmp0(section, "export.header") == 0)
      fe->header = tpl;
    else if (g_strcmp0(section, "export.game") == 0)
      fe->game = tpl;
    else if (g_strcmp0(section, "export.footer") == 0)
      fe->footer = tpl;
    g_free(body);
    g_string_free(raw, TRUE);
    raw = NULL;
  }

#define REQUIRE(sec, key) \
  if (!kv_get(fe, sec, key) || !*kv_get(fe, sec, key)) { \
    rh_err_set(&e, RH_ERR_PARSE, "%s:%d: missing [%s] %s", path, \
               kv_line(fe, sec, key) ? kv_line(fe, sec, key) : 1, sec, key); \
    goto fail; \
  }

  REQUIRE("frontend", "id");
  REQUIRE("frontend", "name");
  REQUIRE("frontend", "layout_rom");
  REQUIRE("frontend", "manifest");
#undef REQUIRE

  if (!fe->header || !fe->game || !fe->footer) {
    rh_err_set(&e, RH_ERR_PARSE,
               "%s: missing [export.header]/[export.game]/[export.footer]", path);
    goto fail;
  }

  {
    static const char *kinds[] = {
      "covers", "screenshots", "videos", "marquees", "miximages",
    };
    for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) {
      const gchar *v = kv_get(fe, "export.media", kinds[i]);
      if (!v || !*v)
        continue;
      RhTemplate *tpl = NULL;
      if (!parse_template(path, "export.media", v,
                          kv_line(fe, "export.media", kinds[i]), &tpl, &e))
        goto fail;
      g_hash_table_replace(fe->media, g_strdup(kinds[i]), tpl);
    }
  }

  g_strfreev(lines);
  g_free(text);
  g_free(section);
  *out = fe;
  return rh_ok();

fail:
  if (lines)
    g_strfreev(lines);
  g_free(text);
  g_free(section);
  if (raw)
    g_string_free(raw, TRUE);
  rh_frontend_free(fe);
  return e;
}

void rh_frontend_free(RhFrontend *fe)
{
  if (!fe)
    return;
  g_free(fe->path);
  if (fe->kv)
    g_hash_table_unref(fe->kv);
  if (fe->kv_line)
    g_hash_table_unref(fe->kv_line);
  if (fe->media) {
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, fe->media);
    while (g_hash_table_iter_next(&it, &k, &v))
      template_free(v);
    g_hash_table_unref(fe->media);
  }
  template_free(fe->header);
  template_free(fe->game);
  template_free(fe->footer);
  g_free(fe);
}

RhErr rh_frontend_find(const char *id, gchar **out_path)
{
  gchar *user = NULL, *sys = NULL;
  RhErr e;

  if (!id || !out_path)
    return rh_fail(RH_ERR_CONFIG, "frontend_find: bad arguments");
  *out_path = NULL;

  if (strchr(id, '/') || g_str_has_suffix(id, ".conf")) {
    gchar *c = g_canonicalize_filename(id, NULL);
    if (rh_fs_is_file(c)) {
      *out_path = c;
      return rh_ok();
    }
    e = rh_fail(RH_ERR_NOT_FOUND, "frontend spec not found: %s", id);
    g_free(c);
    return e;
  }

  user = g_build_filename(g_get_user_config_dir(), "rom-hamster", "frontends",
                          id, NULL);
  gchar *userconf = g_strconcat(user, ".conf", NULL);
  if (rh_fs_is_file(userconf)) {
    *out_path = userconf;
    g_free(user);
    return rh_ok();
  }
  g_free(userconf);
  g_free(user);

  sys = g_build_filename(RH_DATADIR, "frontends", id, NULL);
  gchar *sysconf = g_strconcat(sys, ".conf", NULL);
  if (rh_fs_is_file(sysconf)) {
    *out_path = sysconf;
    g_free(sys);
    return rh_ok();
  }
  g_free(sysconf);
  g_free(sys);
  return rh_fail(RH_ERR_NOT_FOUND, "frontend spec not found: %s", id);
}

/* ============================================================== 渲染 */

static gchar *escape_value(const gchar *v, const gchar *mode)
{
  GString *s;

  if (!v)
    v = "";
  if (g_strcmp0(mode, "xml") == 0) {
    s = g_string_new(NULL);
    for (const gchar *p = v; *p; p++) {
      switch (*p) {
      case '&': g_string_append(s, "&amp;"); break;
      case '<': g_string_append(s, "&lt;"); break;
      case '>': g_string_append(s, "&gt;"); break;
      case '"': g_string_append(s, "&quot;"); break;
      case '\'': g_string_append(s, "&#39;"); break;
      default: g_string_append_c(s, *p);
      }
    }
    return g_string_free(s, FALSE);
  }
  if (g_strcmp0(mode, "json") == 0) {
    s = g_string_new(NULL);
    for (const gchar *p = v; *p; p++) {
      switch (*p) {
      case '"': g_string_append(s, "\\\""); break;
      case '\\': g_string_append(s, "\\\\"); break;
      case '\n': g_string_append(s, "\\n"); break;
      case '\r': g_string_append(s, "\\r"); break;
      case '\t': g_string_append(s, "\\t"); break;
      default:
        if ((guchar)*p < 0x20)
          g_string_append_printf(s, "\\u%04x", (guchar)*p);
        else
          g_string_append_c(s, *p);
      }
    }
    return g_string_free(s, FALSE);
  }
  return g_strdup(v);
}

static gboolean truthy(const gchar *v)
{
  if (!v || !*v)
    return FALSE;
  if (g_strcmp0(v, "0") == 0 || g_strcmp0(v, "0.00") == 0)
    return FALSE;
  return TRUE;
}

static gboolean render_nodes(GPtrArray *nodes, GHashTable *vars,
                             GHashTable *lists, GString *out, RhErr *err);

static gboolean render_loop(RhTplNode *n, GHashTable *vars,
                            GHashTable *lists, GString *out, RhErr *err)
{
  GPtrArray *items = lists ? g_hash_table_lookup(lists, n->text) : NULL;

  if (!items)
    return TRUE;
  for (guint i = 0; i < items->len; i++) {
    gchar *item = g_strdup(g_ptr_array_index(items, i));
    gchar *old_rel = g_strdup(g_hash_table_lookup(vars, "media_rel"));
    gchar *old_file = g_strdup(g_hash_table_lookup(vars, "media_file"));
    gchar *old_kind = g_strdup(g_hash_table_lookup(vars, "media_kind"));
    gchar *old_tag = g_strdup(g_hash_table_lookup(vars, "tag"));

    if (g_strcmp0(n->text, "tags") == 0)
      g_hash_table_replace(vars, g_strdup("tag"), g_strdup(item));
    else {
      gchar *base = g_path_get_basename(item);
      g_hash_table_replace(vars, g_strdup("media_rel"), g_strdup(item));
      g_hash_table_replace(vars, g_strdup("media_file"), base);
      g_hash_table_replace(vars, g_strdup("media_kind"), g_strdup(n->text));
    }

    gboolean ok = render_nodes(n->kids, vars, lists, out, err);

    g_hash_table_replace(vars, g_strdup("media_rel"), old_rel);
    g_hash_table_replace(vars, g_strdup("media_file"), old_file);
    g_hash_table_replace(vars, g_strdup("media_kind"), old_kind);
    g_hash_table_replace(vars, g_strdup("tag"), old_tag);
    g_free(item);
    if (!ok)
      return FALSE;
  }
  return TRUE;
}

static gboolean render_nodes(GPtrArray *nodes, GHashTable *vars,
                             GHashTable *lists, GString *out, RhErr *err)
{
  for (guint i = 0; i < nodes->len; i++) {
    RhTplNode *n = g_ptr_array_index(nodes, i);

    switch (n->type) {
    case NODE_TEXT:
      g_string_append(out, n->text);
      break;
    case NODE_VAR: {
      const gchar *v = g_hash_table_lookup(vars, n->text);
      gchar *esc = escape_value(v, n->escape);
      g_string_append(out, esc);
      g_free(esc);
      break;
    }
    case NODE_IF: {
      const gchar *v = g_hash_table_lookup(vars, n->text);
      GPtrArray *branch = truthy(v) ? n->kids : n->alt;
      if (!render_nodes(branch, vars, lists, out, err))
        return FALSE;
      break;
    }
    case NODE_FOR:
      if (!render_loop(n, vars, lists, out, err))
        return FALSE;
      break;
    }
  }
  return TRUE;
}

static gchar *render_template(RhTemplate *t, GHashTable *vars,
                              GHashTable *lists, RhErr *err)
{
  GString *out = g_string_new(NULL);

  if (!render_nodes(t->nodes, vars, lists, out, err)) {
    g_string_free(out, TRUE);
    return NULL;
  }
  return g_string_free(out, FALSE);
}

/* ============================================================== 导出 */

static gchar *join_extensions(RhPlatform *p)
{
  GString *s = g_string_new(NULL);

  if (!p || !p->extensions)
    return g_string_free(s, FALSE);
  for (guint i = 0; i < p->extensions->len; i++) {
    const gchar *ext = g_ptr_array_index(p->extensions, i);
    if (i)
      g_string_append_c(s, ' ');
    g_string_append(s, ext + (ext[0] == '.' ? 1 : 0));
  }
  return g_string_free(s, FALSE);
}

static gchar *rel_to_prefix(const gchar *abs, const gchar *prefix)
{
  if (g_str_has_prefix(abs, prefix)) {
    const gchar *r = abs + strlen(prefix);
    while (*r == G_DIR_SEPARATOR)
      r++;
    return g_strdup(r);
  }
  return g_strdup(abs);
}

static void vars_set(GHashTable *vars, const char *k, const char *v)
{
  g_hash_table_replace(vars, g_strdup(k), g_strdup(v ? v : ""));
}

static void vars_set_int(GHashTable *vars, const char *k, gint64 v)
{
  gchar *s = g_strdup_printf("%" G_GINT64_FORMAT, v);

  g_hash_table_replace(vars, g_strdup(k), s);
}

static int export_kind_index(const char *kind)
{
  static const char *kinds[] = {
    "covers", "screenshots", "videos", "marquees", "miximages", "intro",
    "patcher",
  };
  for (int i = 0; i < 7; i++) {
    if (g_strcmp0(kind, kinds[i]) == 0)
      return i;
  }
  return -1;
}

int rh_export_kind_index(const char *kind)
{
  return export_kind_index(kind);
}

typedef struct {
  gchar *name;
  gchar *manifest;      /* 绝对路径 */
  gchar *media_dir;     /* 绝对路径 */
  gchar *extensions;
  GString *body;
  gboolean has_game;
} PlatformOut;

static PlatformOut *platform_out_new(const gchar *name, const gchar *manifest)
{
  PlatformOut *p = g_new0(PlatformOut, 1);
  p->name = g_strdup(name);
  p->manifest = g_strdup(manifest);
  p->body = g_string_new(NULL);
  return p;
}

static void platform_out_free(PlatformOut *p)
{
  if (!p)
    return;
  g_free(p->name);
  g_free(p->manifest);
  g_free(p->media_dir);
  g_free(p->extensions);
  g_string_free(p->body, TRUE);
  g_free(p);
}

static gboolean id_selected(RhExportOpts *o, const gchar *id)
{
  if (!o->ids || !o->ids->len)
    return TRUE;
  for (guint i = 0; i < o->ids->len; i++) {
    if (g_strcmp0(g_ptr_array_index(o->ids, i), id) == 0)
      return TRUE;
  }
  return FALSE;
}

static gboolean platform_selected(RhExportOpts *o, const gchar *pl)
{
  if (!o->platforms || !o->platforms->len)
    return TRUE;
  for (guint i = 0; i < o->platforms->len; i++) {
    if (g_strcmp0(g_ptr_array_index(o->platforms, i), pl) == 0)
      return TRUE;
  }
  return FALSE;
}

RhErr rh_export_run(RhCtx *ctx, const RhExportOpts *opts,
                    RhExportResult *result)
{
  RhFilter f = { .multi = -1, .has_patch = -1, .sort = "name" };
  GPtrArray *games = NULL;
  GHashTable *plats = NULL;
  RhPlatformRules *rules = NULL;
  gchar *root = NULL, *dest = NULL, *rootc = NULL;
  RhErr e = rh_ok();

  if (!ctx || !ctx->db)
    return rh_fail(RH_ERR_DB, "export: database is not open");
  if (!opts || !opts->frontend || !opts->dest_dir)
    return rh_fail(RH_ERR_CONFIG, "export: bad arguments");
  if (result)
    memset(result, 0, sizeof(*result));
  if (opts->compress)
    return rh_fail(RH_ERR_UNSUPPORTED, "export: compression is not implemented yet");

  root = g_strdup(rh_config_get(ctx->config, "ROM_ROOT"));
  if (!root || !*root) {
    e = rh_fail(RH_ERR_CONFIG, "export: ROM_ROOT is not configured");
    goto out;
  }
  rootc = g_canonicalize_filename(root, NULL);
  dest = g_canonicalize_filename(opts->dest_dir, NULL);

  gchar *prefix = g_strconcat(rootc, G_DIR_SEPARATOR_S, NULL);
  if (g_str_has_prefix(dest, prefix) || g_strcmp0(dest, rootc) == 0) {
    e = rh_fail(RH_ERR_CONFIG,
                "export: destination must not be inside ROM_ROOT");
    g_free(prefix);
    goto out;
  }
  g_free(prefix);

  if (!rh_fs_mkdir_p(dest, &e))
    goto out;

  rules = platform_rules_new();
  rh_err_clear(&e);
  e = platform_rules_load_standard(rules);
  if (!rh_err_ok(&e))
    rh_err_clear(&e);   /* 规则缺失不阻塞导出，扩展名退化为空 */

  e = store_game_list(ctx, &f, &games);
  if (!rh_err_ok(&e))
    goto out;

  plats = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                (GDestroyNotify) platform_out_free);

  const gchar *emu = rh_config_get(ctx->config, "emulator_launch");

  for (guint i = 0; i < games->len; i++) {
    RhGame *g = g_ptr_array_index(games, i);
    GPtrArray *roms = NULL;
    PlatformOut *po;
    gchar *gdir = NULL, *platform_dir = NULL;
    GHashTable *vars = NULL, *lists = NULL;
    gchar *game_name = NULL, *rom_file = NULL, *block = NULL;
    GString *tagstr = NULL;
    gchar *desc_line = NULL;

    if (!id_selected((RhExportOpts *) opts, g->id) ||
        !platform_selected((RhExportOpts *) opts, g->platform))
      continue;

    gdir = g_build_filename(rootc, g->dir_path, NULL);
    if (!rh_fs_is_dir(gdir)) {
      if (result)
        result->skipped++;
      g_free(gdir);
      continue;
    }

    e = store_rom_list(ctx, g->id, &roms);
    if (!rh_err_ok(&e))
      goto loop_out;
    if (!roms->len) {
      if (result)
        result->skipped++;
      goto loop_out;
    }

    platform_dir = g_build_filename(dest, g->platform, NULL);
    if (!rh_fs_mkdir_p(platform_dir, &e))
      goto loop_out;

    po = g_hash_table_lookup(plats, g->platform);
    if (!po) {
      GHashTable *pv = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             g_free);
      vars_set(pv, "platform", g->platform);
      vars_set(pv, "platform_export_name", g->platform);
      vars_set(pv, "platform_name", g->platform);
      vars_set(pv, "export_dir", dest);
      vars_set(pv, "platform_dir", platform_dir);
      vars_set(pv, "emulator_launch", emu);

      RhPlatform *rp = rules ? platform_get(rules, g->platform) : NULL;
      gchar *exts = join_extensions(rp);
      vars_set(pv, "platform_extensions", exts);

      /* 媒体目录：相对路径按导出根展开 */
      gchar *mtpl = g_strdup(kv_get(opts->frontend, "frontend", "media_dir"));
      gchar *mabs = NULL;
      if (mtpl && *mtpl) {
        RhTemplate *mt = NULL;
        RhErr te = rh_ok();
        parse_template(opts->frontend->path, "frontend.media_dir", mtpl,
                       kv_line(opts->frontend, "frontend", "media_dir"),
                       &mt, &te);
        gchar *v = mt ? render_template(mt, pv, NULL, &te) : NULL;
        template_free(mt);
        mabs = (v && g_path_is_absolute(v)) ? g_strdup(v)
                                           : g_build_filename(dest, v, NULL);
        g_free(v);
      }
      g_free(mtpl);
      if (!mabs)
        mabs = g_build_filename(platform_dir, "media", NULL);

      /* 清单路径：相对路径按导出根展开 */
      gchar *man_tpl = g_strdup(kv_get(opts->frontend, "frontend", "manifest"));
      RhTemplate *mt2 = NULL;
      RhErr te2 = rh_ok();
      gchar *man_raw = NULL;
      if (man_tpl && *man_tpl) {
        parse_template(opts->frontend->path, "frontend.manifest", man_tpl,
                       kv_line(opts->frontend, "frontend", "manifest"), &mt2,
                       &te2);
        man_raw = mt2 ? render_template(mt2, pv, NULL, &te2) : NULL;
      }
      template_free(mt2);
      gchar *man = NULL;
      if (man_raw && g_path_is_absolute(man_raw))
        man = g_strdup(man_raw);
      else if (man_raw)
        man = g_build_filename(dest, man_raw, NULL);
      g_free(man_raw);
      g_free(man_tpl);

      po = platform_out_new(g->platform, man);
      po->media_dir = g_strdup(mabs);
      po->extensions = g_strdup(exts);
      g_free(man);
      g_free(mabs);
      g_free(exts);
      g_hash_table_unref(pv);
      g_hash_table_replace(plats, g_strdup(g->platform), po);
    }

    vars = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    lists = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                  (GDestroyNotify) g_ptr_array_unref);

    /* 主 ROM：多碟优先库内 m3u */
    const gchar *m3u_name = NULL;
    for (guint k = 0; k < roms->len; k++) {
      RhRomFile *r = g_ptr_array_index(roms, k);
      if (g_str_has_suffix(r->filename, ".m3u")) {
        m3u_name = r->filename;
        break;
      }
    }
    rom_file = g_strdup(m3u_name ? m3u_name
                                 : ((RhRomFile *) g_ptr_array_index(roms, 0))->filename);
    gchar *dot = strrchr(rom_file, '.');
    game_name = dot ? g_strndup(rom_file, (gsize)(dot - rom_file))
                    : g_strdup(rom_file);

    /* 先把基础变量填好：下面的模板（layout/media）都要用 */
    vars_set(vars, "platform", g->platform);
    vars_set(vars, "platform_export_name", g->platform);
    vars_set(vars, "platform_name", g->platform);
    vars_set(vars, "export_dir", dest);
    vars_set(vars, "platform_dir", platform_dir);
    vars_set(vars, "media_dir", po->media_dir);
    vars_set(vars, "emulator_launch", emu);
    vars_set(vars, "game_name", game_name);
    vars_set(vars, "rom_file", rom_file);
    vars_set(vars, "name", g->name);

    /* ROM 复制 */
    if (opts->write_roms) {
      for (guint k = 0; k < roms->len; k++) {
        RhRomFile *r = g_ptr_array_index(roms, k);
        if (g_str_has_suffix(r->filename, ".m3u"))
          continue;
        vars_set(vars, "rom_file", r->filename);

        gchar *lay = g_strdup(kv_get(opts->frontend, "frontend", "layout_rom"));
        RhTemplate *lt = NULL;
        RhErr te = rh_ok();
        parse_template(opts->frontend->path, "frontend.layout_rom", lay,
                       kv_line(opts->frontend, "frontend", "layout_rom"),
                       &lt, &te);
        gchar *raw = lt ? render_template(lt, vars, NULL, &te) : NULL;
        template_free(lt);
        g_free(lay);
        if (!raw) {
          e = te;
          goto loop_out;
        }
        gchar *target = g_path_is_absolute(raw) ? g_strdup(raw)
                                                : g_build_filename(dest, raw, NULL);
        g_free(raw);
        gchar *dirn = g_path_get_dirname(target);
        rh_fs_mkdir_p(dirn, &e);
        g_free(dirn);

        gchar *src = g_build_filename(gdir, "rom", r->filename, NULL);
        int rc = rh_fs_copy_atomic(src, target, TRUE, &e);
        g_free(src);
        g_free(target);
        if (rc == 1)
          goto loop_out;
        if (result)
          result->roms++;
      }
      vars_set(vars, "rom_file", rom_file);
    }

    /* m3u：按导出的碟文件重写，避免库内 m3u 指向旧路径 */
    if (opts->write_roms && opts->generate_m3u && roms->len > 1) {
      GString *m3u = g_string_new(NULL);
      for (guint k = 0; k < roms->len; k++) {
        RhRomFile *r = g_ptr_array_index(roms, k);
        if (g_str_has_suffix(r->filename, ".m3u"))
          continue;
        g_string_append_printf(m3u, "%s\n", r->filename);
      }
      gchar *name = g_strdup_printf("%s.m3u", game_name);
      gchar *path = g_build_filename(platform_dir, name, NULL);
      rh_fs_write_text_atomic(path, m3u->str, &e);
      g_free(path);
      g_free(name);
      g_string_free(m3u, TRUE);
      if (!rh_err_ok(&e))
        goto loop_out;
    }

    /* 媒体：复制 + 记录相对路径 */
    {
      static const char *kinds[] = {
        "covers", "screenshots", "videos", "marquees", "miximages",
        "intro", "patcher",
      };
      for (int k = 0; k < 7; k++) {
        gboolean want = opts->media_all || opts->media_kinds[k];
        GPtrArray *media = NULL;
        GPtrArray *rels = g_ptr_array_new_with_free_func(g_free);
        RhTemplate *mt = g_hash_table_lookup(opts->frontend->media, kinds[k]);

        e = store_media_list(ctx, g->id, kinds[k], &media);
        if (!rh_err_ok(&e)) {
          g_ptr_array_unref(rels);
          goto loop_out;
        }
        if (want) {
          for (guint m = 0; m < media->len; m++) {
            RhMediaFile *mf = g_ptr_array_index(media, m);
            if (!mt)
              break;
            vars_set(vars, "game_name", game_name);
            RhErr te = rh_ok();
            gchar *base = render_template(mt, vars, NULL, &te);
            if (!base) {
              e = te;
              g_ptr_array_unref(media);
              g_ptr_array_unref(rels);
              goto loop_out;
            }
            const gchar *ext = strrchr(mf->filename, '.');
            gchar *raw = g_strconcat(base, ext ? ext : "", NULL);
            gchar *target = g_path_is_absolute(raw) ? g_strdup(raw)
                                                    : g_build_filename(dest, raw, NULL);
            g_free(raw);
            g_free(base);
            gchar *dirn = g_path_get_dirname(target);
            rh_fs_mkdir_p(dirn, &e);
            g_free(dirn);
            gchar *src = g_build_filename(gdir, kinds[k], mf->filename, NULL);
            int rc = rh_fs_copy_atomic(src, target, TRUE, &e);
            g_free(src);
            if (rc == 1) {
              g_free(target);
              g_ptr_array_unref(media);
              g_ptr_array_unref(rels);
              goto loop_out;
            }
            g_ptr_array_add(rels, rel_to_prefix(target, platform_dir));
            g_free(target);
            if (result)
              result->media++;
          }
        }
        g_ptr_array_unref(media);
        g_hash_table_replace(lists, g_strdup(kinds[k]), rels);
      }
    }

    /* 单值媒体变量：取每类第一份 */
    {
      static const char *relvars[] = {
        "cover_rel", "screenshot_rel", "video_rel", "marquee_rel",
        "miximage_rel", "intro_rel", "patcher_rel",
      };
      static const char *kinds[] = {
        "covers", "screenshots", "videos", "marquees", "miximages",
        "intro", "patcher",
      };
      for (int k = 0; k < 7; k++) {
        GPtrArray *rels = g_hash_table_lookup(lists, kinds[k]);
        vars_set(vars, relvars[k],
                 rels && rels->len ? g_ptr_array_index(rels, 0) : "");
      }
    }

    /* 元数据变量 */
    char rating[32] = "";
    if (g->rating >= 0)
      g_snprintf(rating, sizeof rating, "%.2f", g->rating);
    char released[32] = "";
    if (g->year > 0)
      g_snprintf(released, sizeof released, "%04d0101T000000", g->year);

    tagstr = g_string_new(NULL);
    GPtrArray *tags = NULL;
    RhErr te = store_game_tags(ctx, g->id, &tags);
    if (rh_err_ok(&te) && tags) {
      for (guint k = 0; k < tags->len; k++) {
        if (k)
          g_string_append(tagstr, ", ");
        g_string_append(tagstr, g_ptr_array_index(tags, k));
      }
      g_ptr_array_unref(tags);
    }

    desc_line = g_strdup(g->description ? g->description : "");
    for (gchar *q = desc_line; *q; q++) {
      if (*q == '\n' || *q == '\r' || *q == '\t')
        *q = ' ';
    }
    g_strstrip(desc_line);

    int disc_no = 0;
    for (guint k = 0; k < roms->len; k++) {
      RhRomFile *r = g_ptr_array_index(roms, k);
      if (r->disc_no > disc_no)
        disc_no = r->disc_no;
    }

    vars_set(vars, "id", g->id);
    vars_set(vars, "name", g->name);
    vars_set(vars, "game_main_name", g->game_main_name);
    vars_set(vars, "description", g->description);
    vars_set(vars, "description_line", desc_line);
    vars_set(vars, "developer", g->developer);
    vars_set(vars, "publisher", g->publisher);
    vars_set(vars, "genre", g->genre);
    vars_set(vars, "region", g->region);
    vars_set(vars, "rom_status", g->rom_status);
    vars_set(vars, "rom_format", g->rom_format);
    vars_set(vars, "dat_source", g->dat_source);
    vars_set(vars, "patch_type", g->patch_type);
    vars_set(vars, "rating", g->rating >= 0 ? rating : "");
    vars_set(vars, "rating_norm", rating);
    vars_set(vars, "releasedate_es", released);
    vars_set(vars, "tags", tagstr->str);
    vars_set(vars, "rom_file", rom_file);
    vars_set(vars, "game_name", game_name);
    vars_set(vars, "platform_extensions", po->extensions);
    vars_set(vars, "is_multi_disc", g->is_multi_disc ? "1" : "");
    vars_set(vars, "is_arcade", g->is_arcade ? "1" : "");
    vars_set_int(vars, "year", g->year);
    vars_set_int(vars, "players", g->players);
    vars_set_int(vars, "disc_no", disc_no);

    block = render_template(opts->frontend->game, vars, lists, &e);
    if (!block)
      goto loop_out;
    g_string_append(po->body, block);
    po->has_game = TRUE;
    if (result)
      result->games++;

    g_free(block);
    block = NULL;

loop_out:
    if (vars)
      g_hash_table_unref(vars);
    if (lists)
      g_hash_table_unref(lists);
    if (roms)
      g_ptr_array_unref(roms);
    if (!rh_err_ok(&e)) {
      g_free(game_name);
      g_free(rom_file);
      g_free(platform_dir);
      g_free(gdir);
      goto out;
    }
    g_free(game_name);
    g_free(rom_file);
    g_free(platform_dir);
    g_free(gdir);
    g_free(desc_line);
    if (tagstr)
      g_string_free(tagstr, TRUE);
  }

  /* 写清单 */
  {
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, plats);
    while (g_hash_table_iter_next(&it, &k, &v)) {
      PlatformOut *po = v;
      if (!po->has_game)
        continue;
      GHashTable *hv = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             g_free);
      GString *out = g_string_new(NULL);
      gchar *h, *ft;
      vars_set(hv, "platform", po->name);
      vars_set(hv, "platform_export_name", po->name);
      vars_set(hv, "platform_name", po->name);
      vars_set(hv, "export_dir", dest);
      vars_set(hv, "emulator_launch", emu);
      vars_set(hv, "platform_extensions", po->extensions);
      h = render_template(opts->frontend->header, hv, NULL, &e);
      ft = render_template(opts->frontend->footer, hv, NULL, &e);
      if (!h || !ft) {
        g_free(h);
        g_free(ft);
        g_hash_table_unref(hv);
        g_string_free(out, TRUE);
        e = rh_fail(RH_ERR_PARSE, "export: failed to render manifest");
        goto out;
      }
      g_string_append(out, h);
      g_string_append(out, po->body->str);
      g_string_append(out, ft);

      gchar *dirn = g_path_get_dirname(po->manifest);
      rh_fs_mkdir_p(dirn, &e);
      g_free(dirn);
      rh_fs_write_text_atomic(po->manifest, out->str, &e);
      g_free(h);
      g_free(ft);
      g_hash_table_unref(hv);
      g_string_free(out, TRUE);
      if (!rh_err_ok(&e))
        goto out;
    }
  }

out:
  if (plats)
    g_hash_table_unref(plats);
  if (games)
    g_ptr_array_unref(games);
  if (rules)
    platform_rules_free(rules);
  g_free(root);
  g_free(rootc);
  g_free(dest);
  return e;
}
