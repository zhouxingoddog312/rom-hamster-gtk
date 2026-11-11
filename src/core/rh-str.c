/*
 * rh-str.c — 字符串工具实现
 */
#include "rh-str.h"

#include <string.h>

gchar *rh_str_trim_dup(const char *s)
{
  gchar *out;

  if (!s)
    return g_strdup("");
  out = g_strdup(s);
  g_strstrip(out);
  return out;
}

gchar *rh_str_lower_dup(const char *s)
{
  if (!s)
    return g_strdup("");
  return g_ascii_strdown(s, -1);
}

gchar *rh_ext_of(const char *path)
{
  const gchar *base, *dot;

  if (!path)
    return g_strdup("");
  base = strrchr(path, '/');
  base = base ? base + 1 : path;
  dot = strrchr(base, '.');
  if (!dot || dot == base)
    return g_strdup("");
  return g_ascii_strdown(dot, -1);
}

gchar *rh_base_noext(const char *path)
{
  const gchar *base, *dot;
  gchar *out;

  if (!path)
    return g_strdup("");
  base = strrchr(path, '/');
  base = base ? base + 1 : path;
  dot = strrchr(base, '.');
  if (!dot || dot == base)
    return g_strdup(base);
  out = g_strndup(base, (gsize) (dot - base));
  return out;
}

gchar *rh_rom_base_name(const char *path, const char *ext)
{
  gchar *base = rh_base_noext(path);      /* 已去掉最后一层扩展名 */
  gchar *ext_copy;
  gchar *last_dot;

  if (!ext || !*ext)
    return base;

  /* 复合扩展名（".p8.png"）：再剥掉前面那一段 */
  ext_copy = g_strdup(ext);
  last_dot = strrchr(ext_copy, '.');
  if (last_dot && last_dot != ext_copy) {
    gchar *lower_base = g_ascii_strdown(base, -1);
    gchar *lower_ext;

    *last_dot = '\0';                    /* 只留复合扩展名的前一段：".p8" */
    lower_ext = g_ascii_strdown(ext_copy, -1);

    if (g_str_has_suffix(lower_base, lower_ext)) {
      gchar *trimmed = g_strndup(base, strlen(base) - strlen(lower_ext));

      g_free(base);
      base = trimmed;
    }
    g_free(lower_base);
    g_free(lower_ext);
  }
  g_free(ext_copy);
  return base;
}

gchar *rh_sanitize_dirname(const char *name)
{
  GString *out;
  const gchar *p;
  gchar *trimmed;
  gint64 chars;

  if (!name)
    return g_strdup("");

  out = g_string_new(NULL);
  for (p = name; *p; p++) {
    switch (*p) {
    case '/': case '\\': case ':': case '*': case '?':
    case '"': case '<': case '>': case '|':
      g_string_append_c(out, '_');
      break;
    default:
      if ((guchar) *p < 0x20)
        continue;                     /* 控制字符直接丢弃 */
      g_string_append_c(out, *p);
    }
  }

  /* 折叠连续空白为单个空格 */
  {
    GString *collapsed = g_string_new(NULL);
    gboolean last_space = FALSE;

    for (p = out->str; *p; p++) {
      if (*p == ' ' || *p == '\t') {
        if (!last_space) {
          g_string_append_c(collapsed, ' ');
          last_space = TRUE;
        }
      } else {
        g_string_append_c(collapsed, *p);
        last_space = FALSE;
      }
    }
    g_string_free(out, TRUE);
    out = collapsed;
  }

  trimmed = g_strdup(out->str);
  g_strstrip(trimmed);
  g_string_free(out, TRUE);

  /* 截断到 180 个字符（UTF-8 码点，不是字节） */
  chars = g_utf8_strlen(trimmed, -1);
  if (chars > 180) {
    gchar *cut = g_utf8_substring(trimmed, 0, 180);

    g_free(trimmed);
    trimmed = cut;
  }
  return trimmed;
}

int rh_parse_disc_no(const char *stem, gchar **out_main)
{
  static gsize re_once = 0;
  static GRegex *re = NULL;
  GMatchInfo *mi = NULL;
  gchar *lower;
  int disc = 0;
  gchar *main_name;

  if (out_main)
    *out_main = NULL;

  if (!stem) {
    if (out_main)
      *out_main = g_strdup("");
    return 0;
  }

  if (g_once_init_enter(&re_once)) {
    re = g_regex_new("(disc|disk|cd|dvd|side)[ _-]*([0-9]+|[a-z])([^a-z0-9]|$)",
                     G_REGEX_CASELESS, 0, NULL);
    g_once_init_leave(&re_once, 1);
  }

  lower = g_ascii_strdown(stem, -1);
  main_name = g_strdup(stem);

  if (g_regex_match(re, lower, 0, &mi)) {
    gchar *token = g_match_info_fetch(mi, 0);
    gchar *num = g_match_info_fetch(mi, 2);

    if (num && *num) {
      if (g_ascii_isdigit(num[0])) {
        disc = (int) g_ascii_strtoll(num, NULL, 10);
      } else {
        disc = num[0] - 'a' + 1;
      }
      if (disc < 1)
        disc = 1;

      /* 用 lower 里找到的位置去原串删同一段，保留原大小写 */
      {
        gchar *pos = strstr(lower, token);

        if (pos) {
          gsize idx = (gsize) (pos - lower);
          gsize len = strlen(token);
          gchar *joined = g_strdup_printf("%.*s%s",
                                          (int) idx, stem,
                                          stem + idx + len);

          g_free(main_name);
          main_name = joined;
        }
      }
    }
    g_free(token);
    g_free(num);
  }
  if (mi)
    g_match_info_free(mi);
  g_free(lower);

  /* 清理残留的括号与空白，例如 "Game (Disc 2)" → "Game" */
  {
    gchar *t = rh_str_trim_dup(main_name);
    gsize len = strlen(t);

    while (len > 0 && (t[len - 1] == '(' || t[len - 1] == '[' ||
                       t[len - 1] == ' ' || t[len - 1] == '-')) {
      t[--len] = '\0';
    }
    g_strstrip(t);
    g_free(main_name);
    main_name = t;
  }

  if (!*main_name) {
    g_free(main_name);
    main_name = g_strdup(stem);
  }
  if (out_main)
    *out_main = main_name;
  else
    g_free(main_name);
  return disc;
}

int rh_name_sim(const char *a, const char *b)
{
  gchar *la, *lb;
  gsize m, n, i, j;
  gint *row;
  int sim;

  la = rh_str_lower_dup(a ? a : "");
  lb = rh_str_lower_dup(b ? b : "");
  m = strlen(la);
  n = strlen(lb);

  if (m == 0 && n == 0) {
    g_free(la);
    g_free(lb);
    return 100;
  }

  row = g_new0(gint, n + 1);
  for (i = 1; i <= m; i++) {
    gint prev_diag = 0;

    for (j = 1; j <= n; j++) {
      gint saved = row[j];

      if (la[i - 1] == lb[j - 1])
        row[j] = prev_diag + 1;
      else
        row[j] = MAX(row[j - 1], row[j]);
      prev_diag = saved;
    }
  }
  sim = (int) ((200 * (gsize) row[n]) / (m + n));
  g_free(row);
  g_free(la);
  g_free(lb);
  return sim;
}
